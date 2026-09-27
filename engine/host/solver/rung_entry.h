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
 *     NAMED (this file) -> CALLED (the member's prologue) -> queued -> the rung was REACHED
 *     (`stepReachedRenderingLife` / `…TimerLife` / `…IdleLife`) -> the RUNG ran (`stepUnitRuns`)
 *   named == 0                           the document hangs nothing off this rung. CORRECT SILENCE, and it is
 *                                        the state no instrument in this engine could express before this one.
 *   named > 0, reached == 0              the ladder never gave the rung a turn. A fact about the arms ABOVE
 *                                        this rung, and NOT about the rung's own component.
 *   named > 0, reached > 0, runs == 0    the rung was reached and declined every time it was asked. A fact
 *                                        about the page's own clock, and the one row of the three at which the
 *                                        rung's component is worth opening.
 * THE `REACHED` RUNG WAS MISSING AND ITS ABSENCE WAS LABELLED `THE FINDING`, WHICH SENT A READER WHERE THE
 * DEFECT IS NOT — AND THIS BANNER HAD ALREADY SAID SO SEVEN LINES ABOVE. The retired row read `named > 0, the
 * rung's runs == 0 — the work was named and the rung never ran. THE FINDING.`, and it is kept in its own words
 * because a reader who re-derives a ladder from `named` and `stepUnitRuns` alone will re-add it. It is
 * VERBATIM the sentence the refusal paragraph above calls `something false, with an address in it`: that
 * paragraph refuses an owed-vs-asked pair taken AT the rung precisely because `the cause may be that the rung
 * sits below an arm that never stopped answering, which is the LADDER's fact`, and then the ladder asserted
 * that cause away. One banner, two statements, and the refusal was the correct one. HOW IT SHOWED: a nonzero
 * `stepNamed…Life` against that rung's `stepUnitRuns` arm at 0 read as THE FINDING and sent a reader to the
 * rung's own component, on a run whose descents had not reached the boundary at all.
 * WHAT CLOSED IT WAS ALREADY BUILT AND PUBLISHED NOWHERE. solver/engine.c raises one arrival count per clock
 * rung, at the arm and ahead of the gate — one for each of this file's three slots, which is why no list of
 * rungs is copied here and why nothing in this component changed to close it. They were a dev-assert operand
 * only, so no reader outside the process could hold one; solver/result.c publishes all three beside
 * `stepUnitRuns` on the same census line.
 * THE RUNGS ARE STILL NOT ASKED IN ISOLATION, AND WHICH ARM ANSWERED INSTEAD IS STILL A READING. `stepUnitRuns`
 * is a PARTITION of every step, so `arms[k] == 0` says that arm never TOOK a dispatch and never says it was not
 * reached, and the arms above a rung are what a reader compares it against off the same line. What has stopped
 * being a reading is the SUFFIX SUM — how many descents got as far as the rung — which is the middle row above
 * and is a row now rather than a sum over the arms at and below that rung, which a reader had to take by hand.
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

/* THE COMPILER RESOLVED `<free identifier>.<member>` — JSConcolicHooks.global_member_named, the sibling of the
   entry above and the same fact about the same occurrence rather than a second occurrence. The report above
   sees `window.requestIdleCallback` as a resolution of `window` and says NOTHING about the member, because a
   field get resolves no scope; this one carries both halves, and it is raised from the same funnel and not from
   the field-get emitter — upstream of the field get there is no receiver, and downstream of it there is no
   compiler.
   WHY THE PAIR AND NOT ONE MORE `…Named…` ROW, WHICH IS WHAT THE RESIDUAL THAT ASKED FOR THIS SAID: the
   `typeof` split does not exist on this spelling. A property of an object is `undefined` when absent and never
   throws, so ECMAScript §13.5.3 step 2.a has nothing to patch and `typeof window.requestIdleCallback` emits
   the same ordinary field get as the read does — the two arrive INDISTINGUISHABLE where the bare spelling
   arrives already separated. Summing this into `…NamedIdleLife` would therefore put a population that is
   largely FEATURE DETECTION into the row read as uses, which inverts the one distinction that split exists
   for; it gets rows of its own, and a reader who wants the union adds two numbers rather than being handed a
   sum somebody else took.
   THE RECEIVER TEST IS THIS FILE'S AND IT IS A SOURCE-TEXT TEST, WHICH IS THE STRONGEST ONE AVAILABLE UPSTREAM
   OF EXECUTION AND IS NO STRONGER THAN A PARSE'S. `base` is tested against the names a realm binds to its own
   global; `api.fetch`, a bundler's `(0,o.requestIdleCallback)` re-export shim and a wrapper's own member are
   refused by that test, and a channel without it would not refine the rows above — it would be a different and
   far louder one. HOW MUCH louder is a property of real bundles rather than of this engine, so it is a command
   and never a number here: `node testing/static_surface.mjs` prints, per declared entry name, the property reads
   that ARE on the global against those that are not, and that figure moves with which bundles were mirrored on
   the day it was taken. The IDENTITY test a
   parse cannot make (is this receiver object the realm's global) needs an OBJECT, so it lives downstream of
   reach, which is the one place a row whose whole purpose is to be upstream of reach may not put its question.
   IT RECORDS AND DECIDES NOTHING, like its sibling. */
void  rung_entry_compile_global_member(const char *base, const char *member);

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
