/* The cooperative slice's bracket and the host's claim on the thread: the two facts about a slice that belong
 * to the host rather than to the scheduler.
 *
 * The slice's budget is engine work (solver/engine.c's engine_quantum_spent against ENGINE_QUANTUM_WORK),
 * counted at dispatch, so it is a function of the flow's path and the same on every host and across a resume.
 * This component owns three things beside it:
 *   - the bracket: whether a slice is open, asserted by every entry that may run only inside or only outside one;
 *   - the host turn: the host's own turn on elapsed time, taken inside a step at the turn points
 *     (quantum_step_turn_point), which moves no rank, account or slice boundary;
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
/* The slice's budget in engine work: ENGINE_QUANTUM_WORK unless a driver states another between slices. It moves
   only where slices end, which no move of a member's stand, no charge and no path depends on, so a driver states
   one to show that (test_forced.c's `--slice-work`). */
void quantum_set_slice_work(int64_t work);
int64_t quantum_slice_work(void);
/* The host's turn, in wall microseconds a slice may hold the thread (0, the default, is never), stated by the host
   between steps: a renderer frame, whose event loop would otherwise run only between two qjs_step calls, states
   one; a native driver is its own process and states one only to show a turn moves nothing. */
void quantum_set_host_turn_us(int64_t us);
/* The host's turn inside a step: an entry the host supplies that runs its own event loop for one turn and
   returns, while the engine's stack waits where the call was made (on a WASM host, an import that suspends the
   instance, main.c's qjs_set_host_turn). Stated between steps, after a turn duration; a host that states none
   takes its turn between qjs_step calls. */
void quantum_set_step_turn(void (*take)(void));
/* Where a step offered the host its turn: the interpreter (every suspend point it offers, and every
   ENGINE_QUANTUM_ASK_EVERY dispatches), host work charged to the running flow, or a residency span's uncharged
   re-derivation. */
typedef enum { QUANTUM_TURN_DISPATCH, QUANTUM_TURN_HOST_WORK, QUANTUM_TURN_RESIDENCY, QUANTUM_TURN_N } QuantumTurnSite;
/* Offers the host its turn after `units` of work at `site`. The clock is read once per ENGINE_QUANTUM_ASK_EVERY
   units offered, a cadence and nothing else; when the hold has lasted the turn, the stated entry runs and the
   step goes on from the same statement. The turn moves no rank, account, work count or slice boundary
   (engine_turn_witness, asserted around it). Outside a slice, or with no entry stated, it does nothing: the thread
   is already the host's, or the host waits for the step's end. */
void quantum_step_turn_point(QuantumTurnSite site, uint64_t units);
/* The turns taken inside a step at `site`, lifetime. A function of elapsed time, so no census carries it. */
long long quantum_step_turns(QuantumTurnSite site);
/* The pricing clock: this thread's CPU in microseconds where the host has a thread CPU clock, its monotonic wall
   clock where it does not (quantum_measure says which). Censuses only. */
int64_t quantum_thread_us(void);
/* What the pricing clock is on this host, as a string a diagnostic prints, and the same fact as a predicate. */
const char *quantum_measure(void);
int quantum_measure_is_cpu(void);
/* `{"sliceWork":…,"price":…,"priceIsCpu":…}`, malloc'd, caller frees; NULL only on allocation
   failure. The slice's budget and this host's clock, both stated outside any slice, so askable inside one or out; result.c composes it as
   `_quantum`, and quantum.c prints it once per instance as the `@QUANTUM` line at the first slice. */
char *quantum_json(void);

#endif
