/* The cooperative slice's bracket and the host's claim on the thread: the two facts about a slice that belong
 * to the host rather than to the scheduler.
 *
 * The slice's budget is engine work (solver/engine.c's engine_quantum_spent against ENGINE_QUANTUM_WORK),
 * counted at dispatch, so it is a function of the flow's path and the same on every host and across a resume.
 * This component owns three things beside it:
 *   - the bracket: whether a slice is open, asserted by every entry that may run only inside or only outside one;
 *   - the host yield: whether the host wants its thread back on elapsed time, which ranks and accounts nothing;
 *   - the pricing clock: this host's thread time, read by censuses that price work (core/timing/thread_work.c,
 *     solver/engine.c's step census) and by no scheduling, ranking, accounting or assert.
 * The slice is a floor on sharing, never a cap: a flow parks as a COW snapshot and resumes byte-identically. */
#ifndef ENGINE_HOST_SOLVER_QUANTUM_H
#define ENGINE_HOST_SOLVER_QUANTUM_H

#include <stdint.h>

/* Open a slice. Exactly one is open at a time: engine_sched_step brackets each slice with these two. */
void quantum_begin(void);
void quantum_end(void);
/* Whether a slice is open: the engine may execute only inside one (preempt_hook asserts it) and the shipped ABI
   may return to the host only outside one (main.c's qjs_step asserts it). An assertion's operand, never a branch:
   a caller that branches on it is choosing between a scheduled and an unscheduled path. */
int quantum_slice_open(void);
/* Whether the host wants its thread back now, on elapsed time since the slice opened. Asked only inside a slice
   (asserted). It moves no rank and no account: the slice's budget is work, and this only hands the thread to the
   host's own event loop. On the emscripten host it answers after QUANTUM_HOST_YIELD_MS, because the renderer
   frame's event loop runs only between two qjs_step calls. On native it never answers: a native engine is its
   own process, the OS shares the machine between processes, and every native host steps straight back in. */
int quantum_host_wants_thread(void);
/* The pricing clock: this thread's CPU in microseconds where the host has a thread CPU clock, its monotonic wall
   clock where it does not (quantum_measure says which). Censuses only. */
int64_t quantum_thread_us(void);
/* What the pricing clock is on this host, as a string a diagnostic prints, and the same fact as a predicate. */
const char *quantum_measure(void);
int quantum_measure_is_cpu(void);
/* `{"sliceWork":…,"hostYieldMs":…,"price":…,"priceIsCpu":…}`, malloc'd, caller frees; NULL only on allocation
   failure. Compile-time constants of this host, so askable inside a slice or out; result.c composes it as
   `_quantum`, and quantum.c prints it once per instance as the `@QUANTUM` line at the first slice. */
char *quantum_json(void);

#endif
