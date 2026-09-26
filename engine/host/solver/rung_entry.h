/* WHAT A PROGRAM NAMES THAT ONLY AN INVOKER RUNG CAN RUN — the denominator `stepUnitRuns` never had.
 *
 * THE DEFECT THIS EXISTS FOR, STATED AS THE READING IT MAKES POSSIBLE. `stepUnitRuns` is a lifetime count of
 * the steps each arm of flow_step's ladder took, and a ZERO in one of its invoker arms stood for three states
 * that take opposite work: the document hangs nothing off that rung, the ladder never gave the rung a turn, or
 * the rung's hook is not installed. A grep settles the third. The first two are merged, and they are merged for
 * the reason a DOOR's zero is merged — the arm is an `else if` inside flow_step, so REACHING IT IS RUNNING and
 * a census there is a census of an OUTCOME downstream of reach. §AN-INVARIANT-OVER-A-GATED-OPERATION says the
 * ASK is recorded upstream of every arm that may legitimately decline, and the ladder is nothing but declining
 * arms, so no counter inside it can ever separate the two. This one is not inside it: it is raised where the
 * COMPILER resolves a free identifier against the global object, which is the last instant this engine knows
 * what a program NAMES independently of what any flow RAN.
 * MOVING THE RECORDING POINT INTO THE LADDER WAS PROPOSED AND IS REFUSED, AND THE REFUSAL IS KEPT BECAUSE IT IS
 * THE DESIGN A READER RE-DERIVES: an owed-vs-asked pair taken AT the rung reports "the page hangs work off this
 * rung and the rung never ran", which a reader acts on by going to the rung's own component — while the cause
 * may be that the rung sits below an arm that never stopped answering, which is the LADDER's fact. A zero says
 * nothing; that pair would say something false, with an address in it. This file is the other half of the pair
 * and states nothing about why a rung was not reached.
 * AND NO ASSERT COULD HAVE STOOD HERE EITHER, which is why this is a census and not a `DCHECK`. An occurrence no
 * flow reached has no frame, no operand and no moment. At the END of a run a frame exists, and the only
 * invariant that would fire is "this document asked for something", whose operand is a count derived from the
 * PAGE'S OWN BYTES — the one operand §WHOSE-BYTES-STATE-THE-VALUE forbids, since a page does not have to be
 * hostile to hold an abort switch and only has to ship no `requestAnimationFrame`.
 *
 * THE LADDER THE PAIR COMPLETES, whose LOWEST ZERO IS THE LOCALISATION:
 *     NAMED (this file)  ->  CALLED (the member's prologue)  ->  queued  ->  the RUNG ran (`stepUnitRuns`)
 *   named == 0                        the document hangs nothing off this rung. CORRECT SILENCE, and it is the
 *                                     state no instrument in this engine could express before this one.
 *   named > 0, the rung's runs == 0   the work was named and the rung never ran. THE FINDING.
 * THE RUNGS ARE NOT ASKED IN ISOLATION AND THE PAIR DOES NOT SAY WHICH ARM ANSWERED INSTEAD. `stepUnitRuns` is
 * a partition of every step, so the arms ABOVE a rung are on the same line and are what a reader compares it
 * against; that is a reading and not a row, and it belongs to whoever holds both numbers.
 *
 * ONE RUNG TAKES MANY NAMES AND THAT IS WHY THE DECLARATION IS A LIST. A single `entry` per rung was the shape
 * first proposed and it is wrong for two of the three: solver/engine.c's own rendering arm names
 * `requestAnimationFrame`, ResizeObserver delivery, IntersectionObserver, scroll/resize/pagereveal and the Web
 * Animations checkpoint as work that rung runs, and §8.7 "Timers" puts BOTH `setTimeout` and `setInterval` on
 * one task source. A one-name column would have published a denominator that was a fraction of its own rung.
 *
 * THE NAMES ARE THE COMPONENT'S AND ARE NOT DERIVED FROM `browser/platform_names.h`, WHICH WAS THE OTHER
 * CANDIDATE AND IS A DIFFERENT KIND OF ARTIFACT. That header is a generated FLAT ARRAY of every global name Web
 * IDL exposes on Window — a SET with no key, so a component cannot cite a member of it by name, and citing one
 * by INDEX would be §AN-INDEX-NAMES-A-THING-ONLY-WHILE-THE-SET-IS-FIXED over a set re-sorted at every regen
 * from @webref. Its PURPOSE is the opposite question too: its own banner says absent.c reads it to tell a Web
 * API this engine OWES from server-injected app state, so it lists a name whether or not this engine installs
 * it — and whether this engine installs it is exactly the fact a denominator needs.
 * SO THE SECOND COPY IS REMOVED AT ITS SOURCE RATHER THAN BY IMPORTING A THIRD ARTIFACT: a component spells its
 * entry name ONCE, as a named constant, and uses that ONE constant at its `idl_install_method` AND in the table
 * it declares here. Drift is then impossible rather than merely unlikely, which is §Fix-the-ROOT's own test —
 * an assert that the declared name resolves on the global would be a check on a state that can no longer arise.
 *
 * SCOPE IN TIME IS THE INSTANCE'S AND NOT A SESSION'S, WHICH IS WHAT MAKES THE PAIR COMPARABLE AT ALL. Nothing
 * here is reset: `g_step_unit_runs` is released by nothing (solver/engine.c says so at its own declaration), so
 * a denominator scoped to a session would be graded against a numerator scoped to the instance — the defect
 * `g_boundary_spent` exists to catch, one census over. solver/endpoint.c's `…AskNamed…` rows take the OTHER
 * scope for the same reason: they are read against ask rows that endpoint_init resets.
 * A REPORT AND NEVER A BOUND (§NO BOUNDS): nothing branches on one of these, no construction is refused because
 * of one, and no arm is narrowed by one. */
#ifndef ENGINE_HOST_SOLVER_RUNG_ENTRY_H
#define ENGINE_HOST_SOLVER_RUNG_ENTRY_H

#include "solver/step_unit.h"

/* A COMPONENT DECLARES THE FREE GLOBAL IDENTIFIERS WHOSE WORK ITS RUNG RUNS. `names` is a NULL-TERMINATED table
   of names with STATIC STORAGE, borrowed and never copied, exactly as an endpoint edge lends its stage table:
   the census keys rows on it for the life of the instance, and a name added to the component's own table needs
   no edit here. Called from the component's per-realm install, so a second call with the SAME table is the
   ordinary case and costs nothing; a DIFFERENT table for one rung is two components claiming one arm's
   denominator and crashes rather than summing two populations under one name.
   A RUNG WITH NO ROW MAY NOT DECLARE, asserted here rather than discovered as a silence: the row names are
   literals (core/json_buf.h's `json_buf_key` takes one and the compiler enforces it), so a rung this file has
   no key for would be counted and published nowhere — a write with no reader, which is the defect
   §A-FIELD-A-CONSUMER-DEFAULTS names and the one a census is least able to show. */
void  rung_entry_declare(StepUnit u, const char *const *names);

/* THE COMPILER RESOLVED A FREE IDENTIFIER AGAINST THE GLOBAL OBJECT. Reached through the one seam that reports
   it (JSConcolicHooks.global_named) and NOT installed directly on that table, because it is the second consumer
   of one fact: solver/concolic.c assembles the table and dispatches to both, which is where the list of
   consumers belongs and is the same split it already makes to solver/absent.c.
   `name` is bytes valid for this call only and `typeof_only` says which of the two reads it was — an ordinary
   reference, or the non-throwing form the unary parser patches in for `typeof` (ECMAScript §13.5.3 "The typeof
   Operator" step 2.a answers an unresolvable reference without throwing). It records and decides nothing. */
void  rung_entry_compile_global_named(const char *name, int typeof_only);

/* The rows on the heap (caller frees; NULL only on allocation failure). ROWS AND NOT A CENSUS OF THEIR OWN, for
   the reason solver/endpoint.h gives for the edge rows: they are spliced into `_cold` beside `stepUnitRuns`,
   each with a LEADING comma, because a reader compares WITHIN a census — and a nested census would have needed
   a name in extension/bridge.js's relay list and a row in extension/popup.js's, which are TRUSTED-ZONE
   JavaScript live on WRITE while this half is live only after a build.
   NUMERIC ROWS AND NOT ONE HISTOGRAM OVER `StepUnit`, which was the other shape. A full-extent histogram would
   publish a structural zero for every arm that has no denominator at all, and a reader could not tell that from
   an arm whose denominator IS zero — absent read as zero, which is the defect this file exists to end arriving
   in its own output. A sparse histogram is honest and is an OBJECT row, which engine/build.mjs's `censusRowSet`
   refuses until a READING is written for it there; that reading is a real obligation and not this diff's.
   THE EMPTY STRING IS THE ABSENT FORM AND IS NOT SIX ZEROES — a host whose realms install none of these
   components has no population, and §Testing's rule is that an absent count and a zero count are different
   facts that must never be averaged. The rows and the comma in front of them go together. */
char *rung_entry_rows(void);

#endif /* ENGINE_HOST_SOLVER_RUNG_ENTRY_H */
