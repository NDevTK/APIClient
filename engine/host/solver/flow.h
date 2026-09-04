/* FLOW + WFQ — the scheduler's unit of work, rebuilt clean and FRAME-AGNOSTIC.
 *
 * A FLOW is a code-flow through the program: a DECISION VECTOR over the shared pre-boot baseline. Its state is
 * replay(baseline, decision vector). The engine explores by re-running flows and forking at each concolic
 * branch — and CRUCIALLY, a fork is expressed as "append an arm to a decision vector," which is identical
 * whether the branch was a bytecode OP_if or a native builtin loop-back. That is the whole point of the
 * rebuild: the old design coupled forking to a bytecode OP_if rewind, so native frames could not fork; here a
 * flow is just (fn, decision vector), frame-agnostic by construction.
 *
 * The WFQ orders flows by an anytime-bandit priority: accumulated emitted VALUE + a UCB optimism bonus
 * (∝ 1/(1 + service) so a never-run flow is never starved) − CPU aging (a monopolizer that burns CPU without
 * emitting sinks below productive+unrun flows). ORDER-only: it never drops a work item.
 * THE MONOPOLIZER IS A FORK CHAIN, NOT A FLOW, which is why the AGING term reads the FAMILY's service and not
 * the flow's: an arm is one of N names one exploration wears, the reward is copied to every one of them, and
 * billing each name separately lets the chain age forever without sinking. See the `cpu`/`family`/`acct` fields
 * and flow.c's FlowAcct. */
#ifndef ENGINE_HOST_SOLVER_FLOW_H
#define ENGINE_HOST_SOLVER_FLOW_H

#include <lexbor/dom/dom.h>

#include "quickjs.h"
#include "solver/world.h"
#include "solver/pending.h"   /* the replies the host still owes this flow — a JS Array, not a malloc'd list */
#include "solver/dyn_body.h"  /* …and the program text its sequence holds — shared, because no flow writes it */
#include "solver/step_unit.h" /* …and which arm of flow_step it last returned through — a NAMED value, not a
                                 free-form string, so the census row and the abort text are one list */

/* A NODE OF THE FORK TREE, AND THE ONLY THING IN A FLOW THAT OUTLIVES IT. It exists for one sentence: the thread
   time a DEPARTING flow burned has to reach the flow that FORKED it, because §scheduler prices the aging term
   against a monopolizer and a fork chain is one monopolizer wearing N names. OPAQUE here on purpose — a rank
   input has exactly one reader (flow.c's flow_weight), and a field nothing outside can spell is a field no other
   subsystem can grow a second ranking out of. See flow.c for the refcounting and the charge. */
typedef struct FlowAcct FlowAcct;

/* §@S'S LADDER, NAMED WHERE THE FITNESS TERM IS DEFINED RATHER THAN WHERE IT IS OBSERVED. The rungs are
   ORDERED stages toward a fire and the comparator is composed from them (flow_distance), so the count of them
   is arithmetic in the weight and cannot be a convention two files agree on separately.
   THE FIRE IS NOT ONE OF THEM. §@S names {filter-survived, sink-reached, context-escaped, handler-fires}; the
   last is the OUTCOME, and §@S(i)'s whole objection to the old fitness is that rungs sitting at or past the
   thing they are a distance to are "the outcome restated". A fire closes the search and is a finding — the
   LEDGER's quantity — so the comparator orders the candidates that have NOT fired.
   AND THE FIRST RUNG IS BELOW EVERY ONE §@S NAMES, because every one it names reports AT A SINK and a
   candidate spends most of its life on the runway getting to one. §@S(i) asks for an observation site
   "strictly before the thing it is a distance to", and read against the sink rungs alone that condition was
   satisfied by nothing: `filter-survived` is measured on a string a sink was handed, so a candidate 800 gates
   into its replay and one that has not yet reached its own SOURCE READ both stand at exactly 0 and rank
   identically for the whole of the runway. The one honest observation available in between is the DELIVERY —
   the substitution actually being performed, which is the moment the attacker's bytes become a value in the
   page's own program. It is not "the page has been reached" and not "the payload is downstream of something":
   deciding the second needs a taint tracker or a recorded transform-expression, both of which §Re-execution
   bans, while the first is a fact the component that performs the substitution holds already.
   SO THE LADDER IS FOUR RUNGS AND ITS SITES ARE STRICTLY ORDERED: the delivery (a source read), then the
   survival fraction and the two sink booleans (a sink write). Its range is still exactly 1.0 — one fractional
   rung plus three booleans over a denominator of four — which is what prices it against the optimism term so
   a PROMISE never outweighs a FINDING (flow.c's flow_weight), and the derivation of flow.c's
   flow_silence_us_to_sink reads FLOW_RUNGS_N rather than a literal, so it moved with this and did not have to
   be redone.
   THE FRACTIONAL RUNG SITS BELOW THE DELIVERY IN THE ARITHMETIC AND ABOVE IT IN TIME, AND THAT IS NOT A
   CONTRADICTION — IT IS UNREACHABILITY, ASSERTED. `flow_distance` composes `cand_surv + cand_rung`, so a
   nonzero fraction under rung 0 would rank a flow that has delivered nothing beside one that has. No such
   flow exists: a survival fraction is measured only on bytes that entered the program, so the delivery is
   already recorded when it is taken, and flow_observe_survival asserts exactly that rather than trusting it. */
#define FLOW_RUNG_DELIVERED 1 /* this flow's payload was substituted into the page's own program — a SOURCE read */
#define FLOW_RUNG_ARRIVED   2 /* this flow's breakout reached the sink its own search is for */
#define FLOW_RUNG_ESCAPED   3 /* …and stood there in a position the sink's own language executes */
#define FLOW_RUNGS_N        4 /* the fractional rung plus the three above it — the comparator's denominator */

/* WHAT ONE STEP OF A FLOW ANSWERED. OWED is not a third kind of flow — it is the same flow reporting that the
   work it has left belongs to the host, so the scheduler can tell an exhausted frontier from a waiting one
   without any member leaving the queue. */
#define FLOW_STEP_MORE  0
#define FLOW_STEP_DONE  1
#define FLOW_STEP_OWED  2

typedef struct Flow {
    /* THIS FLOW'S WORLD — its name in the ONE timeline it owns, valid in every document it touches. `delta`
       below is only this instance's SEGMENT of that world; a flow that scripts an iframe or a popup writes in
       another WASM instance, and that instance keys ITS segment by this id. A delta cannot travel (it names
       its targets by live heap pointers), so the name is what crosses — see solver/world.h.
       IT CHANGES AT EVERY BRANCH THIS FLOW TAKES, and that is the point rather than an oversight: a fork
       RETIRES the world it branched at and mints a child for BOTH arms, so the name a peer holds a segment for
       is always a world no flow will write from again. A flow that kept its name across a branch would be
       indistinguishable, at a peer, from the arm that diverged from it. */
    WorldId world;
    JSValue fn;            /* the function this flow re-drives (JS_UNDEFINED for a boot/session flow) */
    /* THE DECISION VECTOR IS NOT A FIELD HERE, and the flat `signed char *dec` + `dec_n` that used to be is
       DELETED. It was the from-baseline replay mechanism — a birth vector a flow would replay from cursor 0 —
       and no caller ever supplied one, so it had exactly one prospective user (the cold tier's resume) and was
       the wrong shape for it: a flat per-flow array is the quadratic decide.c's shared chain deleted, and a
       park that wrote one per flow would multiply the sharing back out on the way to disk. A flow's vector
       lives in `dec_blob` below — the shared frozen chain — whether it was frozen by a suspend, by a fork, or
       rebuilt by the cold tier from a recipe. ONE representation, so a resumed flow and a forked one are the
       same kind of thing to everything downstream. */
    /* WHAT THIS ONE MEMBER HAS EMITTED (new @H + @S), ONE POINT PER EMISSION — A CENSUS QUANTITY AND NOT A
       RANK, WHICH IS THE CORRECTION THIS FIELD CARRIES. It used to be "the WFQ's reward term" and it was also
       copied at every fork and at every from-baseline arrival, and those two sentences cannot both be true of
       one number: a term COPIED AT AN INSTANT differs between two arms of one parent by whatever the parent
       emitted between their two branches, which is birth order and not merit, and it is unbounded while every
       other term of the weight has a range of at most 1.0 — so it dominated, and an unbounded term read
       descending on birth order serves newest-first. Measured on the smoke fixture: a frontier growing roughly
       twenty-four fold inside a frozen reward band a hundred and sixty-eight points wide, its floor never
       moving off its second reading, and not one member reaching the tail.
       §scheduler'S REWARD IS THE FORK FAMILY'S NOW (flow.c's FlowAcct `val`, reached by flow_reward below), at
       the SAME accounting unit the aging that cancels it is charged to. This field is what the account cannot
       say: which MEMBERS are producing. A frontier retiring members steadily while every one of them reads
       zero here is one coasting on an ancestor's findings — the question `val_born` was invented to ask by
       subtraction, and there is nothing to subtract once nothing is inherited, so that field is DELETED rather
       than kept beside this one.
       NEVER COPIED, NEVER INHERITED, NEVER READ BY flow_weight. A fork's precondition asserts it is still zero
       (flow.c's flow_fork_inherit), which is what keeps it a measurement: the moment it is assigned at a fork
       it is a rank again, and a rank a fork copies is the defect above. */
    double val;
    /* THE AGING TERM, AND ITS UNIT IS THE WHOLE OF WHETHER THE TERM WORKS. Thread time in MICROSECONDS burned
       since this flow's FORK FAMILY last emitted — never a step/opcode/visit count. A count is not commensurate with `val`
       above: a step used to be a whole drain and became one unit of work, so the same charge billed a flow the
       same amount for twelve milliseconds of execution as for advancing a script index, and §scheduler's
       sentence ("a monopolizer that burns CPU without emitting sinks below productive AND unrun flows") could
       not be true at any rate expressible in steps. In microseconds the two terms share a currency and the
       exchange is stated once, at FLOW_AGE_RATE.
       int64 rather than long because `long` is 32 bits in wasm: 2147 seconds of unproductive CPU would overflow
       it, and a NEGATIVE cpu makes the monopolizer the highest-ranked flow in the frontier — the exact failure
       this term exists to prevent, arriving silently after 36 minutes.
       AND IT IS WHAT THE AGING READS AGAIN, BESIDE THE FAMILY'S — which is the correction this field carries,
       and it is the reverse of the one it carried before. It read: "`cpu` is the OPTIMISM term's quantity and
       only that — §scheduler's bonus is '∝ 1/(visits+1)', visits are this flow's own turns at the thread". A
       MICROSECOND IS NOT A VISIT, and reading it as one put the same physical quantity — thread time — into
       BOTH of flow_weight's non-reward terms at two scopes. §scheduler names three quantities (emitted value,
       visits, CPU) and the weight then held two, so the term priced to demote a monopolizer was DOMINATED by
       the term meant to protect a newcomer: FLOW_AGE_RATE prices one second of CPU at one emitted finding —
       the optimism bonus's ENTIRE range — while `1/(1+cpu/Q)` spent HALF that range in ONE QUANTUM, so the
       priced budget was dominated eighty-three to one and governed nothing, and no flow on a heavily-forking
       document ever held the thread long enough to finish the program it was inside. What the rate buys is a
       flow's REWARD in seconds and not a fixed grace period: the term steps in cooperative quanta (flow.c's
       FLOW_AGE_QUANTUM), so a flow tied with an unrun sibling on reward and bonus hands over after ONE quantum
       — which is the queue rotating — and a flow that has emitted V findings holds the thread for V seconds of
       silence before that sibling reaches it, which is the rate doing what it is priced for. `visits` below is the optimism term's quantity now; this
       one is the aging's own half, and the family's is the other. Both are reset by an emission credited to
       this flow's ACCOUNT, which is what makes them AGING (silence) rather than lifetime service.
       IT IS A QUANTITY ONLY BESIDE `cpu_gen`, AND THAT IS THE CORRECTION THIS FIELD CARRIES. It used to be the
       burn since THIS FLOW's own last emission — a strictly longer window than the family's, since a flow's own
       emission is one of its family's — and the two halves were summed into one notch and weighed against a
       reward credited over the family's window. That is a unit error rather than a policy, and it had a
       population: this field is written only for the flow HOLDING THE THREAD, so a member the scheduler has
       never dispatched carried whatever its parent had burned at the fork for the life of the frontier, while
       the parent's was forgiven at the parent's next emission. The arm's deficit was then repayable only by the
       dispatch the deficit itself was foreclosing — §scheduler's razor's STARVES, and not a deprioritisation.
       READ IT THROUGH flow.c's flow_own_silence AND NEVER RAW. The field is a reading only while `cpu_gen`
       matches the account's `emit_gen`; past an emission it holds a previous window's arithmetic. Every reader
       in the tree goes through that function — the weight, both notches, the census rows, flow_pick's guard,
       the arrival rule — so the field and its mark cannot be separated by an edit that touches one of them.
       WHY BOTH HALVES AND NOT ONE. The family charge alone cannot order anything WITHIN a family, because every
       arm reads the identical number — and a real page's whole frontier is one family (every flow descends from
       the boot flow through flow_fork_inherit), so §scheduler's "a monopolizer that burns CPU without emitting
       sinks below productive+unrun flows" was a statement with no comparison left in it exactly where the
       monopolizer is. The own half is what makes it true among siblings; the family half is what keeps a fork
       chain one accounting unit against OTHER families (see `family` below, and the 8910-against-1124 reading
       that put it there). Neither replaces the other. */
    int64_t cpu;
    /* WHICH SILENCE WINDOW `cpu` ABOVE IS A READING OF — this flow's copy of its account's `emit_gen`, and the
       half of that quantity without which the other half is a number rather than a measurement.
       IT IS INHERITED AT A FORK exactly as `cpu` is, and for the identical reason: an arm is its parent's path
       with one more arm on it, so it stands at the parent's silence in the parent's window. Copying the burn
       and not the mark would make an arm of a just-emitted parent read that parent's PREVIOUS window's burn as
       its own — a fork carrying a debt the parent had already been forgiven — and flow_fork_inherit's
       rank-neutrality equality is what fires on it.
       WHY A GENERATION AND NOT A WALK. An emission forgives the whole account's window, so every member of the
       family must read zero own-silence from that instant; a frontier reaches thousands of members and
       flow_weight is O(1) by construction (it is evaluated inside DCHECK conditions, where a walk that
       compresses is forbidden), so the forgiveness is one increment on the account and a comparison at each
       read. The normalisation of the stale field happens where the flow is CHARGED (flow.c's
       flow_age_running), which is a write site already and where the write is observationally a no-op. */
    uint64_t cpu_gen;
    /* §scheduler'S "visits", WHICH IS A COUNT OF COMPLETED UNITS OF WORK AND NOT A CLOCK — the optimism term's
       whole quantity. A flow is credited a visit when a scheduler step leaves it BETWEEN units: not inside a
       program and holding no parked continuation, which is HTML §8.1.4.4 "Calling scripts"'s "if the JavaScript
       execution context stack is now empty" and is the same predicate the microtask checkpoint is placed against
       (engine.c). A flow preempted in the middle of a program has not completed a trial and is not credited one.
       THAT IS THE WHOLE OF WHY THE JOB PUMP WAS STARVED, and it is arithmetic rather than a story. flow_step
       can only reach a flow's queued jobs with `frame == NULL` — every job arm is under that test — so a
       reaction runs only after its holder has finished the program it is inside. With the bonus keyed on thread
       time, a flow was strictly outranked by every arm it had forked the moment it crossed one 12 ms quantum,
       and each of those arms was in turn outranked by the arms IT forked inside its own first quantum: the pool
       of members at notch 0 was refilled by branching faster than a quantum drained it, so no flow ever received
       a SECOND quantum, no program ever ended, and no microtask ever ran. Measured on one real login page: 4650
       flows, 3093 switches — under one turn per flow — with 217 jobs queued and ZERO run over ninety seconds,
       against a page whose whole fetch surface hangs off promise reactions. Keyed on completed units the same
       flow keeps the thread until it finishes the program, its arms then tie with it and take their turns, and
       the reactions run. It is not a job priority and there is no job in it: the term counts units of work, and
       a program is one.
       INHERITED AT A FORK for the same reason `cpu` is — an arm has, by construction, completed every unit its
       parent completed before the branch. flow_fork_inherit's rank-neutrality DCHECK is what forces the
       inheritance, and it fires the moment it is forgotten.
       AND **NOT** PLACED AT AN ARRIVAL, WHICH IS THE OPPOSITE CASE AND USED TO BE THE SAME ONE. The two doors
       look alike and their premises are contradictory: a fork IS its parent's execution with one more arm on
       it, while flow_add_unseeded routes a flow to the arrival precisely BECAUSE it stands on nobody's
       decisions. So the fork's "by construction" reasoning is exactly false of an arrival, and the arrival was
       copying the count of whichever flow happened to hold the thread — a fact about a stranger. That is the
       two-instants test failed at a second door: two newcomers arriving at two instants read 1/9 and 1/14 for
       units NEITHER completed. What it cost is §scheduler's guarantee at the only door that guarantee is
       about — "a UCB optimism bonus proportional to 1/(visits+1) so a NEVER-RUN FLOW IS NEVER STARVED" is a
       sentence whose whole content is this term's value at zero, and every from-baseline flow (the @S
       candidate session, a joined document's boot flow, a cold-resumed recipe) was born at a tenth of it.
       Measured: twelve candidate sessions at one reward for a whole run, `turns:0` on every one.
       THE TERM IS THEREFORE OUTSIDE THE ARRIVAL'S COORDINATE ENTIRELY (flow.c's flow_optimism, split out of
       flow_queue_weight beside §@S's distance for the identical sentence — a reading of the flow is not a
       position in the queue). A newcomer ties the flow in service on the coordinate and stands one optimism
       range above it on the weight, which is the rank §scheduler assigns a flow that has completed nothing and
       is bounded at one emission by flow_nonreward. It pays that back at FLOW_AGE_QUANTUM per quantum burned.
       AND NOT RESET BY AN EMISSION, WHICH IS THE ONE THING ABOUT THIS FIELD THAT USED TO BE A QUESTION AND IS
       NOW SETTLED. It used to be written to zero on the EMITTER, on the reasoning that a flow which has just
       produced something is not one the frontier needs protecting from and "then leads by its REWARD". That
       reasoning was written when `val` was a per-flow field: the reward is the fork FAMILY's, so within a
       family it leads nobody, and the zero was residue of the era when this term read `cpu` and forgiving the
       silence and forgetting the trials were one statement in one field.
       WHAT SETTLED IT IS THE TEST THIS ACCOUNTING ALREADY APPLIES TO EVERY TERM: two arms of one parent, forked
       at two instants, must be worth the same, because neither did anything between the two branches. A zero
       written on the emitter is COPIED by every fork it takes afterwards, so two arms straddling one emission
       stood a whole optimism range apart for an event NEITHER performed and BOTH were already credited for
       through the account they share — one credit minted once and presented twice, which is exactly what
       FlowAcct's `val` was moved onto the family to stop. And the field that WOULD hold a per-member emission
       preference already exists and is deliberately unranked: `Flow.val` is what one member emitted and is
       never read by flow_weight, so the zero was that refusal being overturned through this term instead.
       SO THIS COUNT IS ONE QUANTITY WITH ONE MEANING — units of work completed on this flow's own prefix,
       raised only by flow_credit_visit, carried by a fork, and monotone for the life of the flow. It is the
       ONE term of the weight with no reset of any kind, which is worth stating beside `cpu` rather than left to
       be re-derived: `cpu` above is forgiven for the whole ACCOUNT at an emission (FlowAcct's `emit_gen`)
       because it is weighed against a reward the account earns, and this one is weighed against nothing — it
       is a trial count, and a trial that happened cannot un-happen.
       A PARK DOES NOT CARRY IT, AND THAT IS CONSISTENT RATHER THAN AN OMISSION. The cold tier writes the
       account's reward and not this (cold.c), so a resumed member re-enters at zero; it then REPLAYS its
       document and re-completes the units its recipe describes, which is the same reason §@S's distance is
       "re-earned rather than resumed across a park". A count that is re-earned by the work that earned it is
       not a reset. */
    int64_t visits;
    /* HOW MANY TIMES THE SCHEDULER HAS HANDED THIS MEMBER THE THREAD — a CENSUS quantity and never a rank, in
       the same class as `val` beside it: it is not read by flow_weight, it is not inherited at a fork, and it
       is not reset by anything. It exists because §scheduler's word for the state the razor forbids is
       STARVES, and no other field in this struct can name the population that sentence is about.
       EVERY CANDIDATE FOR THAT ROLE IS A TERM OF THE WEIGHT, AND flow_credit_emit RESETS THE SILENCE HALVES OF
       ALL OF THEM. The own-silence half goes to 0 for every member of the emitting family, so the census row
       built on it counts a flow that has just produced something AND every arm standing beside it (build.mjs's
       reader says so in as many words); the family notch goes to 0 on any arm's emission, so flow_pick's own
       `unrun` population — every non-reward term at zero — is documented at its site as non-empty only within
       one quantum of an emission, which is why its §ONE-WFQ guards short-circuit to vacuity on
       exactly the frontier where a member is being starved. A count of DISPATCHES is the one statement none of
       that can move: a member that was never switched in was never given the chance to emit, so nothing it
       could have done can erase the fact.
       `visits` USED TO BE ON THAT LIST AND IS NOT ANY MORE, WHICH SHORTENS THE ARGUMENT WITHOUT WEAKENING IT.
       An emission zeroed the emitter's count, so "completed no unit of work" counted it too; that write is gone
       (flow_credit_emit says why it was one credit presented twice), and `vis_zero` below now means what it
       reads. This field is still the answer, because the two rows ask different questions: `vis_zero` counts
       members that finished nothing, and a member can finish nothing for a whole run BECAUSE it was dispatched
       into a program that never ends — which is a resume-seam defect and not an ordering one. Only a dispatch
       count separates those, and it is the separation the row exists for.
       IT IS WHAT SEPARATES TWO DIAGNOSES THAT TAKE OPPOSITE WORK, and until it existed a run could not tell
       them apart at all: a world missing from the emitted surface because its flow was never picked is an
       ORDERING defect, and one missing because its flow was picked and made no forward progress is a defect in
       the resume seam. Measured, on the shipped artifact, with a four-line document — a gate whose taken world
       emits and then enters an opaque-length walk loses the other world's endpoint entirely under `direct`,
       `preempt` and `eager` alike — and the census could name neither cause.
       NOT INHERITED AT A FORK, and that is what keeps it a measurement rather than a rank: an arm is its
       parent's path with one more arm on it for every term the ORDER reads, and it is a brand-new work item
       for the question "has the scheduler ever chosen this". flow_fork_inherit asserts it is still zero when
       the account is handed over, for the same reason it asserts `cpu` and `visits` are: a sibling that had
       already been dispatched was run at a weight nobody chose. Because a fork does not carry it, its
       rank-neutrality DCHECK is also what forbids this field from ever entering flow_weight. */
    int64_t picks;
    /* THE OTHER HALF OF WHAT THE AGING TERM READS — the root of this flow's fork family, shared by every arm of it, holding
       the thread time the whole family has burned since any of its arms last emitted. A from-baseline flow founds
       one (it points at its own `acct`); a FORK joins its parent's (flow_fork_inherit), which is what makes "a
       fork chain is one monopolizer wearing N names" true of the arithmetic: the reward is stated once per
       family and copied to every name, so the aging that cancels it is charged once per family too.
       MEASURED BEFORE IT EXISTED: `svcMax` 1124 notches against `svcFamMax` 8910 on a 686-member frontier — the
       family had burned 7.93x what its largest single arm showed, against `valMax` 10.0 and an optimism range of
       1.0, so every from-baseline flow (every @S candidate session, every joined document's boot flow, every
       cold-tier resume) waited on N separate arms each paying the family's whole reward off alone. `candSvcMax`
       was 1: the security search had one quantum in fifteen minutes.
       IT IS NOT THE ANCESTRY. `acct`/`up` below records who forked whom and is what keeps this root addressable;
       this is a DIRECT pointer at the root so flow_weight costs one indirection rather than a walk — which it
       must, because flow_weight is evaluated inside DCHECK conditions where a compressing find is forbidden. */
    FlowAcct *family;
    /* THIS FLOW'S PLACE IN THE FORK TREE — minted with the flow, attached under its parent's by
       flow_fork_inherit, and refcounted so the family root above stays addressable for exactly as long as any
       descendant can still read it. See flow.c. */
    FlowAcct *acct;

    /* INTERLEAVING STATE — persisted while this flow is PAUSED so the scheduler can run another flow and come
       back. A flow is preempted mid-execution (cooperative quantum) and resumed byte-identically; its COW
       delta, decision cursor, and pins all swap with it (see engine.c). Zero-initialized by flow_add. */
    /* A CANDIDATE SESSION. This flow re-runs the page with one attacker payload substituted for one source, to
       see whether it FIRES at the sink. It is not a different KIND of flow — same scripts, same scheduler, same
       preemption — it just carries the substitution, which is why the candidate lives here rather than in a
       driver that runs the program start-to-finish beside the BFS. NULL for an ordinary flow. */
    char *cand_src;        /* the source identity the payload replaces (owned) */
    char *cand_payload;    /* the breakout to try (owned) */
    const char *cand_sink; /* the sink name to record if it fires (static) */
    /* DID THIS FLOW'S PoC FIRE, and is its substitution live? Both were globals in solve.c, which is only
       correct while one candidate runs start-to-finish with nothing else scheduled — the shape the standalone
       verify driver has and the BFS does not. As a flow among flows a candidate is preempted, parked and
       resumed with ordinary flows in between, so a global `fired` records another flow's marker and a global
       `verifying` leaves the substitution live for whoever runs next. They belong to the flow, and they swap
       with it. */
    /* AND NEITHER CROSSES THE COLD TIER, which is a decision recorded where the fields are rather than only at
       the writer. `cand_verifying` is not independent state at all — solve_flow_begin sets it from `cand_src`
       on every switch-in, so a parked flow re-derives it before it runs an opcode. `cand_fired` is DROPPED on
       purpose: a candidate can fire and then be preempted before it finishes, and carrying the bit would let
       solve_flow_end record a PoC on the strength of a fire the resuming session never saw — while a replay is
       not obliged to reproduce one, since §Time-travel has it re-deriving values from CURRENT sources. §@S:
       only firing proves it, so the replay re-observes or nothing is recorded. See cold.c's park_rec_cand. */
    int cand_fired;        /* this flow's X9 marker executed */
    int cand_verifying;    /* this flow is a candidate run: the sink takes the concrete arg */
    /* WHERE THIS CANDIDATE'S PAYLOAD CAME FROM — the park record it was rebuilt out of (1), or this session's
       own search list (0). It exists so that "these bytes have no row in this session's record" is a POSITIVE
       statement rather than a hole a reader fills with a guess.
       §@S ALREADY SAYS THE FACT AND NOTHING STATED IT. A cold-resumed candidate's payload "rides the resumed
       FLOW rather than this session's record" (solve.h, on `payloads` being empty beside a non-zero `tried`),
       so solve.c's arrival check cannot decide from the payload list alone whether an unlisted payload is a
       resumed one — which is legitimate — or a candidate assembled outside both doors, which is the assembly
       failure that check exists to catch. Those are opposite verdicts and the list reports them identically.
       IT IS RE-DERIVED AND NEVER PARKED, exactly like `cand_verifying` above it: a candidate that parks again
       is written as a 'c' record and comes back through the same rebuild arm, which sets this again. Nothing
       reads it before that arm runs — solve.c reads it only at a sink, and a flow reaches a sink by running. */
    int cand_resumed;      /* this candidate was rebuilt from a park record, so its payload has no row here */
    /* HOW FAR THIS CANDIDATE'S OWN BYTES HAVE GOT — §@S's fitness read as a COMPARATOR, which is a different
       quantity from the reward beside it and is the half `val` structurally cannot be.
       A REWARD IS A LEDGER AND A FITNESS IS A COMPARATOR, and the two obey opposite rules. A ledger records
       what a search has LEARNED, so it must be paid at most once for one observation — a second payment for
       ground already covered reorders the frontier on nothing. A comparator states where an item stands NOW,
       so it must be readable off EVERY item, including the ones standing exactly where an earlier item already
       stood. Pay a distance into the ledger and the rule that keeps the ledger honest is precisely the rule
       that erases the comparison: the first candidate of a search to travel nine tenths of the way is paid for
       it and every later candidate that travels the same nine tenths is worth what an unstarted flow is worth.
       §@S's "a near-miss is mutated toward the gap; a dead candidate starves" is then true of nothing — not
       because the distance is unmeasured, but because the only place it was written was a ledger.
       SO THIS IS NEVER ACCUMULATED AND NEVER PAID. It is where THIS flow's own bytes have been observed to
       stand on §@S's ladder, in [0,1], overwritten upward and read at the pick. It cannot double-count because
       there is nothing to count: two flows at the same distance simply compare equal, and one that falls
       behind another is passed. It shares the optimism term's entire range, so it is priced against the same
       aging and buys a candidate the same order of thread time a never-run flow gets.
       AND IT IS THE WHOLE LADDER RATHER THAN ITS FIRST RUNG, WHICH IS THE HALF THAT WAS MISSING AND THE HALF
       THAT MATTERS MOST ON THE SINKS THAT MATTER MOST. §@S names four rungs — {filter-survived, sink-reached,
       context-escaped, handler-fires} — and only the FIRST of them was ever written here; the other two
       pre-fire rungs were paid into the search-level LEDGER at a 0->1 crossing and nowhere else. Read the two
       rules above against that arrangement and the consequence is exact rather than theoretical: a page that
       does not transform the payload delivers it whole, so the survival fraction is 1.0 for EVERY candidate of
       that search the moment its bytes surface anywhere, and the comparator is then a CONSTANT across the one
       population it exists to order. The candidate whose breakout arrived at its own sink and left the state it
       was written into ranked exactly equal to the one whose bytes turned up in some unrelated string and
       stopped — while the ledger, obeying its own honest rule, had already paid the single crossing to whichever
       flow got there first and had nothing left for either. So the rungs are recorded HERE, per flow, and the
       distance is composed from them; the ledger keeps its crossings and is untouched.
       THE FIRE IS NOT ONE OF THE RUNGS, and excluding it is §@S(i) rather than an omission: a fitness whose
       rungs sit AT the thing they are a distance to "is not a distance at all, it is the outcome restated".
       Firing closes the search (solve.c's record_sink) and is a FINDING, which is the ledger's quantity; the
       comparator's job is to order the candidates that have NOT fired, and every rung it reads has its
       observation site strictly before that.
       IT IS CARRIED BY A FORK for the reason every weight term is: an arm of a candidate is that candidate's
       run continued under one more arm, carrying the same payload to the same sink, so a fork that dropped it
       would let a candidate improve its own rank by branching. flow_fork_inherit's rank-neutrality assertion is
       what forces that and fires the moment it is forgotten — and it is written over the WHOLE weight rather
       than over a list of fields precisely so that splitting this quantity into the two fields below could not
       be done without the fork carrying both.
       IT DOES NOT CROSS THE COLD TIER, and that is the same decision `cand_fired` records one field up: a
       distance is an OBSERVATION of a re-execution, and a resumed session has not made it. A parked candidate
       comes back at zero and re-earns it from its first arrival, which is what keeps the number a measurement
       of this session's runs rather than a rank inherited from a run nobody watched. */
    /* RUNG ONE, HELD AS THE FRACTION IT IS: the best fraction of this flow's own payload that any re-execution
       has been observed to deliver to any code-execution sink, in [0,1]. It is the only rung that is not a
       boolean, because "how much of what the page was given is still alive" has degrees and the other two do
       not. */
    double cand_surv;
    /* …AND THE BOOLEAN RUNGS, AS A COUNT RATHER THAN AS BITS, because they are ORDERED and a count is what
       makes the order the arithmetic instead of a convention: 0 = this flow's bytes are not in the program at
       all, FLOW_RUNG_DELIVERED = the substitution has been performed at a source read, FLOW_RUNG_ARRIVED = the
       breakout built out of it reached the sink its own search is for, FLOW_RUNG_ESCAPED = and it stood in an
       executable position there. A flow cannot hold one without its predecessor — every escape site in solve.c
       runs downstream of the arrival site on the same string, and every arrival site runs downstream of a
       delivery because a candidate arm returns at the sink's door unless its substitution has happened — and
       flow_observe_rung asserts that rather than trusting it.
       0 IS THEREFORE A POSITIVE STATEMENT AND NOT A "NOT YET", which is the whole reason the bottom rung is
       worth a rung: it separates a flow the ordering has never served, or that a gate killed on the runway,
       from one whose bytes are in the program and were eaten by a filter. Those took the same number before
       and take opposite actions — the first is a scheduling or path question, the second a breakout one. */
    int cand_rung;

    /* IS THIS FLOW A DRIVEN ORPHAN — a flow whose frame is a CALL of a function the page defined and nothing
       ever called (engine.c's engine_orphan_seed). It is not a different KIND of flow in any way the scheduler
       can see: same assembly as an answer-fork arm, same delta, same world, same rank, same preemption, and its
       branches fork ordinary siblings. It carries exactly one consequence, and the field exists to state it:
       ITS WORK IS NOT IN ITS RECIPE. Every other flow's recipe is (decision vector, reward) and a resume
       re-runs the DOCUMENT under it, which reproduces the flow — but re-running the document is precisely what
       never calls this function, so a resumed orphan flow would be a document replay wearing an orphan's rank
       and the drive would be silently gone. What closes that is the FUNCTION LOCATOR below: the recipe carries
       the decision vector like every other flow's AND a name for the function, and the resumed flow drives the
       body that name matches instead of taking a fresh one. Inherited by a fork, because an arm of an orphan
       drive is the same drive continued and is no more replayable than its parent. */
    int orphan;
    /* WHERE THAT FUNCTION IS, WRITTEN AS SOMETHING A LATER SESSION CAN FIND — quickjs's JS_OrphanHash of the
       script the body was compiled from, its position in that script, and its own source text. `fn` above is a
       live heap reference and dies with the session; this is what crosses the tier, so it is stamped when the
       drive is created and carried unchanged by every arm of it. 0 for a flow that is not a driven orphan,
       which is the one value the hash is never asked to mean and is asserted as such at the park. */
    uint64_t orphan_hash;
    /* IS THIS DRIVE STILL WAITING FOR ITS FUNCTION — set only by the cold tier's rebuild, because a drive with
       no call frame is the one thing only a resume can produce. A resumed session's heap does not hold the
       function at the instant the residue lands: the closure is created by the DOCUMENT'S OWN REPLAY, and it
       becomes reachable at the moment some flow takes it as an orphan. Until then this flow replays the
       document like any other member, and the two states it passes through are told apart by `fn` itself,
       which is what `fn` means: UNDEFINED while it is still waiting, and the re-taken function once one has
       been handed to it. The flow then builds its own call frame — in its OWN timeline, never in the timeline
       of whichever flow happened to take it, because the receiver and the arguments are concolic objects and an
       object minted under another flow's stamp is that flow's private state for the rest of the session. */
    int orphan_want;
    /* AND HOW MANY UNKNOWNS THAT CALL HAS TO SUPPLY — the callee's own declared formal parameter count, handed
       over by the take beside the function. Live state and NOT part of the recipe: it is a fact about the body
       this session compiled, so the session that resumes reads it off the take rather than trusting a number
       an older one wrote down. Meaningful only while `orphan_want` is set and `fn` is a function. */
    int orphan_argc;

    /* HAS THIS FLOW'S RECIPE BEEN WRITTEN TO THE PARK DOCUMENT? A paged flow is not a dropped one — that is the
       whole claim the cold tier makes — and it is a fact about THIS FLOW rather than about the session. It used
       to be asked of the engine (`engine_frontier_paged`), which is true only of the whole-frontier park: a
       PARTIAL self-park writes the lowest-value TAIL and releases it while the engine keeps running on its top
       flows, so an engine-wide answer would excuse every later release of a flow that was never written down.
       `flow_release` reads it (a continuation parked on a flow whose recipe exists is replayed next session; one
       on a flow that was not written is dropped), and `cold_park_flow` sets it and refuses to write a flow that
       already carries it — which is the same statement the old once-per-session park assert made, said per flow
       so that it stays exact when the park runs several times. */
    int paged;

    /* HAS THIS FLOW TOLD THE SCHEDULER IT CAN MAKE NO PROGRESS? A flow answers FLOW_STEP_OWED when the only
       thing left to it belongs to the HOST — a fetch not yet answered, a document script whose text has not
       arrived, a synchronous cross-instance read the peer has not resolved. The scheduler must not hand it the
       thread again until something could have changed, and the only alternative to recording that per flow is
       what stood in its place: a COUNT of consecutive owed answers, broken at `flow_count()`. That count is a
       no-progress bound in §NO-BOUNDS' own list, and it was not even a correct one. The WFQ re-picks the SAME
       top-ranked flow — an owed step burns microseconds, so its aging does not move a service notch and its
       weight does not move at all — so N owed answers were N answers from ONE flow, and the loop then declared
       the whole frontier stalled while runnable siblings had never once been asked. That is the razor's
       "starves, skips" exactly: in the smoke host the provider then answers nothing, run_scheduler breaks, and
       every one of those flows dies unexplored with the result document reporting a clean drain.
       IT IS A GENERATION STAMP, NOT A FLAG, so clearing every mark is one increment rather than a walk of a
       frontier that has reached tens of thousands of members — see flow_clear_host_owed. */
    unsigned owed_gen;

    /* HAS THIS FLOW A RECORDED PATH TO STAND ON? 0 = fresh: decide_enter gives it an empty vector and every
       branch it meets is a new decision. 1 = it resumes from the blobs below — which is the snapshot-forked
       sibling (a live frame plus its chain), and equally the flow the COLD TIER rebuilt from a recipe (no
       frame, cursor 0, replaying its recorded arms as it re-runs the document from its first script). Those
       two are deliberately one state: a resumed flow is not a third kind of flow, it is a flow whose decision
       state was rebuilt somewhere other than a fork. */
    int   started;
    /* WHICH ARM OF flow_step THIS MEMBER LAST RETURNED THROUGH — solver/step_unit.h owns the list and says why
       the answer had to move onto the flow. The scheduler stamps it at the ONE point every step converges on,
       so it cannot go missing when an arm is added; what the arm itself declares is only its own name. It
       decides NOTHING — a pure record, read by the `@COLD` histogram and by the seamless-stretch aborts — and
       it is `STEP_UNIT_NONE` on a calloc'd member, which is the true answer for a flow the pick has never
       reached rather than a hole a reader has to guess at.
       IT DOES NOT CROSS THE COLD TIER, and that is the same decision `cand_surv` records two screens down: it
       is an observation of a step THIS session took, so a rebuilt flow reads `none` until this session steps
       it — which is the honest answer and not a lost field. A parked unit carried forward would report a
       resumed frontier under the arms of the session that parked it. */
    StepUnit step_unit;
    void *frame;           /* the current script's live preemptible frame (JS_FlowNew handle), NULL between scripts */
    /* IS THAT FRAME THE ROW'S PROGRAM, OR THE REPORT THE ROW'S PROGRAM OWES?
     *
     * HTML §8.1.4.4 "Calling scripts", run a classic script step 8's third bullet reports an abrupt completion
     * BEFORE step 8.3.2's clean up — which is where the microtask checkpoint is — so the report is the flow's
     * very next work and takes the frame slot the program has just vacated. It is a CONTINUATION OF THE SAME
     * ROW and not a row of its own, which is the whole content of this flag: while it is set, `script_i` still
     * names the script that threw, and the three things a program's completion does belong to the report's
     * completion instead of to the throw's.
     *
     * WITHOUT IT ALL THREE ARE WRONG AND TWO OF THEM ARE SILENT. `script_i++` at the throw would advance past
     * the row while the report is still standing on it, so the report's own completion would advance a second
     * time and the flow would SKIP the next script — a work item dropped, which §scheduler's razor forbids.
     * §4.12.1.1 "Processing model"'s execute-the-script-element runs the classic script at step 3 and restores
     * `document.currentScript` at step 4, and the report is INSIDE step 3, so a page's `error` listener reads
     * the throwing `<script>` element from `document.currentScript`; restoring at the throw would hand it null.
     * And a report frame that itself completes abruptly is a should-never-happen — reporting a report is what
     * §8.1.4.6 step 6's error-reporting-mode flag exists to stop — which cannot be told from a page script's
     * throw without this.
     *
     * `JS_FlowIsCall` CANNOT ANSWER IT. A report frame is a call root and so is a driven orphan's, and an
     * orphan flow runs ordinary programs too (a lazy chunk it loaded is a row of its sequence), so a flow can
     * hold either kind of call frame with the same `orphan` bit set. The two completions mean opposite things
     * — an orphan's throw is this engine's invocation on unknown input and is nobody's defect; a report's is a
     * defect in this engine — so the fact has to be written down at the moment it is true.
     * Carried by a fork like every other field of the flow: an arm branching inside an `error` listener is
     * standing in the same report its parent is. It is NOT cold-tier state — a resumed flow replays the
     * document, the script throws again and the report is owed again. */
    int   reporting;
    /* IS THIS FLOW'S LIVE FRAME A MODELLED CLOSE REQUEST'S TASK — `reporting`'s question about a different
     * kind of frame, and asked for the same reason: the frame is a CALL ROOT, so `JS_FlowIsCall` cannot tell it
     * from a driven orphan's call or from a report, and the three completions mean opposite things. An orphan's
     * throw is this engine's invocation on unknown input and is nobody's defect; a report's is a defect in this
     * engine; and this one is a step of HTML §6.10.1 "Close requests" throwing, which the standard's own text
     * makes impossible for the page to cause (a close action "can never throw an exception", and DOM §2.9's
     * inner invoke step 2.11 catches a listener's). Its NORMAL completion is a value that has to be read, which
     * is the third thing the other two do not have.
     * NOT COLD-TIER STATE, for `reporting`'s reason: a resumed flow replays its document and re-reaches its own
     * exhaustion, where the arrival is modelled again. Carried by a fork, because an arm branching inside a
     * watcher's `cancel` handler is standing in the same task its parent is. */
    int   close_req;
    /* …AND WHETHER A MODELLED CLOSE REQUEST IN THIS TIMELINE ALREADY REACHED §6.10.1's STEP 9. "Alternative
     * processing: Otherwise, there was nothing watching for a close request." — the standard's own statement
     * that this document, in THIS flow's delta, has nothing for a close request to do, and therefore the one
     * fact that lets the arrival stop being modelled without a counter, a cap or a seen-set. It is not a bound:
     * membership of the frontier is untouched, the page may establish a watcher at any later instant, and a
     * flow that closed something is asked again immediately (§6.10.2's process close watchers takes ONE group,
     * so a document with three of them is modelled three times, in one flow, as three tasks).
     * WRITTEN FROM THE TASK'S COMPLETION VALUE and nowhere else, which is why `close_req` above exists at all.
     * Carried by a fork like every other field of the path, and NOT cold-tier state: the manager it is a fact
     * about is per-flow COW state that a resumed flow rebuilds by replaying its document. */
    int   close_req_none;
    /* HAS THIS FLOW'S PATH EVER TAKEN AN ARM THE CONCRETE EXAMPLE CONTRADICTED — the DERIVED/FORCED
     * discriminator, and the one fact a request this flow builds cannot state without it.
     *
     * CLAUDE.md §A-REQUEST-CARRIES-THE-PROVENANCE names three provenances and requires every outbound request
     * to declare which it is. Two of them are facts about the PARK — a parser-inserted `<script src>` is named
     * by bytes the trusted zone itself fetched — and the third is a fact about the PATH: "a value exists only
     * because a gate was forced". A value can be perfectly concrete and still have been reached only through a
     * branch this run took AGAINST what the example says, and the request built from it is then evidence about
     * what a server answers to a request no client makes. That is not derivable from any value: the value is
     * the same bytes either way, so the discriminator has to be recorded where the ARM was taken, which is
     * here, beside the flow's other path state.
     *
     * IT IS THE CONTRADICTION AND NOT THE FORK. Forced multi-path forks both arms of every branch over an
     * unknown, so "this flow forked" is true of nearly every flow and separates nothing. What separates is
     * whether the flow's own arm disagrees with the concrete example the value carries — §Solver-half's "at a
     * branch the example marks the real arm" — and a branch over a value with NO example contradicts no
     * observation and marks nothing.
     * IT IS NOT ONE WRITER AND THIS SENTENCE SAID IT WAS. What stood here was "decide.c is the single writer
     * (there is exactly one place an arm is taken)", and that was a claim about THIS TREE rather than about the
     * concept — the kind that goes stale the moment a second producer lands, and it had. What the field
     * actually records is broader than a branch and always was: EVERYTHING THIS PATH COMPUTES FROM HERE ON
     * RESTS ON SOMETHING NOTHING OBSERVED. Three acts put a flow in that position, and each states so at its
     * own site through the one entry point:
     *   - a BRANCH whose arm the concrete example contradicts (solver/decide.c) — the original, and the only
     *     one that is an "arm" in the field's own name;
     *   - a REQUEST THE TRUSTED ZONE DECLINED, whose failure arm runs the page's `catch` over an outcome this
     *     engine supplied and nobody sent (solver/engine.c's flow_decline_fork);
     *   - a MODELLED POTENTIAL CLOSE REQUEST, an arrival no user performed (solver/engine.c's close-request
     *     arm; core/html/close_request.h says why it is forced rather than fabricated).
     * The list is here because the ONE writer is `flow_mark_forced_arm` and a reader of this field has to be
     * able to find every act that reaches it; a fourth producer is a line in this list, not a second bit.
     *
     * MONOTONE, because a path cannot un-take an arm: once a flow stands past a contradicted branch,
     * everything it computes afterwards stands on it. That is also why it can be a bit rather than a count —
     * a count would be a second quantity with no reader, and the WFQ must never read either: this is not a
     * weight term, and a flow that branched cheaply must not be able to change its rank by having done so.
     * Carried by a fork exactly like every other field of the path (flow_fork_inherit states it and asserts
     * it), because an arm is its parent's path with one more arm on it.
     *
     * NOT COLD-TIER STATE, for `reporting`'s reason one line up: a resumed flow REPLAYS its recorded arms
     * through the same decide.c seam, so every contradiction it stood on is re-observed as it is re-reached —
     * and re-observed against TODAY's examples, which §Time-travel-resume requires ("a resumed flow re-derives
     * example VALUES from CURRENT sources"). A serialized bit would state last session's answer about this
     * session's server. */
    int   path_forced;
    /* THE DOCUMENT'S LOAD STAGE IS NOT HERE, and the field that was is DELETED. One integer cannot hold N
       documents: an agent is an origin-keyed CLUSTER, so a flow reaches several Documents and HTML gives each
       its own readiness and its own DOMContentLoaded. The stage lives on each Document (document.c's readiness
       slot), which is a heap write the COW delta already isolates per flow — so it is still per-flow, and it is
       now also per-document, which is what it always had to be. */
    int   script_i;        /* position in this flow's ONE program sequence: a row of `dyn`, on [0, dyn_n) */
    /* ONE SEQUENCE AND ONE ADDRESS SPACE. The cursor used to walk the SESSION document's own scripts out of a
       separate table on [0, n) and only then this flow's rows on [n, n+dyn_n), through an offset every reader
       restated. The document's scripts are seeded as rows of `dyn` at creation now (flow_set_seed_hook), so
       there is no half to be in — which is also what makes §4.12.1.1's "immediately execute the script element"
       expressible at every position rather than only past the document's last <script>.
       A POSITION MAY BE A SCRIPT WHOSE SOURCE HAS NOT ARRIVED, and the flow STOPS at it — which is what gives
       every document of this agent §4.12.1's order rather than the order its replies happen to land in. The
       row is this flow's own (engine.c's DYN_SCRIPT_SRC), holding the address until the reply replaces it with
       the program; one host fetch still answers every flow parked on that address, because engine_provide
       fills every register that names it. */
    /* THE HIGHEST SCRIPT INDEX THIS FLOW HAS COMPILED, so that compiling one twice can be caught. A flow runs
       each program in its sequence ONCE; a preempted flow RESUMES its suspended frame and never re-enters
       JS_FlowNew for a program it already started. Re-compiling is a REPLAY, which this engine does not do — a
       replay re-executes side effects the flow already performed against a delta that already holds them. */
    int   last_compiled;   /* -1 until the flow compiles its first program */
    /* WHERE A RUN OF INTERPOSED PROGRAMS HAS REACHED, so that a SECOND interposition at one slot goes BEHIND
     * the first instead of in front of it. HTML §4.12.1 "The script element"'s "prepare the script element"
     * ends "Otherwise, immediately execute the script element el, even if other scripts are already
     * executing", and in a browser that run happens INSIDE the causing program — so two elements one program
     * prepares run in the order it prepared them. This engine expresses "inside" as the slot after the cursor,
     * and that expression ALONE reverses them: both interpositions compute the same slot, and the second
     * shifts the first down. `document.write("<script>a()</script><script>b()</script>")` and
     * `body.appendChild(s1); body.appendChild(s2)` are the same two lines of that defect.
     *
     * THE WITNESS IS THE BASE SLOT, NOT THE CURSOR, AND THAT IS WHAT MAKES THE PAIR SELF-VALIDATING. The base
     * is `script_i + 1` inside a program and `script_i` between programs (`frame` is exactly "inside a
     * program"), so the two programs that can stand at one cursor — the row itself, and a job running before
     * it — have DIFFERENT bases, and a stale pair can only be reused by a program whose interpositions really
     * do belong behind the ones already there. There is therefore no invalidation site to remember, which
     * matters because the cursor is advanced from more than one place.
     *
     * NO PER-ROW COLUMN WOULD DO, which is the shape this was nearly built as. "Which rows did the RUNNING
     * program interpose" cannot be read off `dyn_pos`: a row an ANCESTOR interposed carries DYN_POS_IMMEDIATE
     * too and sits in the same run, so a scan that skipped every immediate row would put a grandchild's
     * program behind its parent's sibling — the same reversal one level up.
     *
     * A FORK CARRIES IT, like every other field of the parent's history: an arm is its parent's timeline
     * continued, standing at the same cursor over the same rows, with the same interpositions already made. */
    int   imm_at;          /* the base slot `imm_next` was computed from; -1 while no run is open */
    int   imm_next;        /* the slot the next IMMEDIATE row of that run takes */
    /* this flow's OWN program bodies (per-flow, not global): a lazily-loaded chunk, a queued document script —
       or, while the kind beside it is DYN_SCRIPT_SRC, the ADDRESS of a script whose source has not arrived. One
       column either way, because it is one queue and the entry is one position in it.
       THE ROW IS THIS FLOW'S; THE BYTES ARE NOT. A program's source text is fixed the moment it is decoded and
       no flow can write it, so it is shared baseline state and every timeline holding that program holds the
       SAME buffer (solver/dyn_body.h). It was a `char *` this table strdup'd, and the fork therefore cost
       O(total script bytes) rather than O(rows) — a 2.1 MB module bundle is an ordinary size for one real
       single-page app, forced multi-path execution forks per branch, and the run ended at the allocator with
       the page's whole learned surface as nothing. This is the same conversion solver/pending.h records for
       the register beside it, on the one column that was left. */
    DynBody **dyn; int dyn_n, dyn_cap;
    /* WHICH DOCUMENT each of those programs belongs to, which is WHERE it is compiled — a program is closed
       over the compiling realm's global (JS_FlowNew), and an instance is an ORIGIN-KEYED AGENT CLUSTER, so the
       document a program belongs to is a child navigable's as often as it is the session's. §7.4 step 14's
       load hands a same-origin child its own realm and that document's classic scripts are the CREATING FLOW's
       next programs; compiled in the session's realm they would run against the creator's Window, defining the
       child's globals on the parent and reading the parent's back as the child's.
       A DOCUMENT HANDLE AND NOT A JSContext*: a handle survives a park and a realm does not, and a queued
       program outlives the turn that queued it. It is also where a cross-agent operation's document lives —
       there was a `perform_doc` beside the token on the flow saying the same thing, and this is the field that
       already said it. Parallel to `dyn` for the reason
       `dyn_cand` is — a field added to the queue is an obligation at every clone, free and finish site, and the
       seven arrays are allocated, copied and freed together so one that got a field the others did not cannot
       stay unnoticed. */
    uint32_t *dyn_doc;
    /* AND THE RENDEZVOUS TOKEN OF THE PEER PARKED ON IT, for the one kind of row that OWES AN ANSWER. A
       cross-agent operation's answer IS its program's completion, so the question and the program are one thing
       and the token is a fact about the ROW. Held beside the flow instead it was a single slot, and both halves
       of that were wrong: a second operation arriving before the first had answered had nowhere to go, and two
       operations that differ ONLY in the asking WORLD — same verb, same document, same member, which is what
       route.mjs phase 3 asks twice — are indistinguishable by every other thing a row carries, so no amount of
       re-deriving from the cursor could have paired them. NULL for every other kind, and NULL again the moment
       the answer is sent; a DYN_CROSS_AGENT_OP row with no token is a peer suspended forever, which is why the
       kind and the token are written together at the one queue entry allowed to create that kind. */
    char **dyn_token;
    /* WHAT KIND each of those programs is (a DynKind, engine.c). A page script that does not compile is a real
       problem and asserts; the two other kinds are ORDINARY when they do not. An @S CANDIDATE that does not
       compile is the common case — most breakouts do not fit most sink contexts, which is why the solver tries
       several and keeps the one that FIRES, and CLAUDE.md names it: an unsolved @S candidate is a parked search,
       never a @WHY. A `javascript:` URL that does not compile is HTML §7.4.2.3.2's abrupt evaluation, which
       simply produces no Document. Kept as a parallel array so the page-script assert stays fully armed inside a
       candidate flow, which still loads real chunks. */
    unsigned char *dyn_cand;
    /* AND WHICH OF HTML §8.1.4.4 "Calling scripts"'s TWO ALGORITHMS RUNS IT (a ScriptType, core/loader/
       document_scripts.h). §4.12.1.1 "Processing model"'s "execute the script element" ends in a switch on the
       ELEMENT's type — "classic" runs the classic script, "module" runs the module script — and a queue with no
       column for it could only ever answer one of the two. The consequence was not a subtlety: three separate
       seams (core/frame/navigable.c's child-navigable Document, engine.c's engine_join_document, and
       core/html/html_script.c's INJECTED element) each aborted outright on `<script type=module>` rather than
       compile a module as a classic program, which would have come back a SyntaxError from a parser that is
       perfectly correct. This column is what those three asserts were asking to exist.
       CLASSIC IS A STATEMENT ABOUT A ROW, NOT A DEFAULT: a `setTimeout` string, a `javascript:` URL, a lazy
       chunk and a cross-agent operation's program are classic scripts because that is what §8.1.4.4 evaluates
       them as. Only a row an ELEMENT put there can say MODULE, which is why the two entry points that take one
       (engine_queue_element_script / engine_queue_docscript_url) are the only ones with the parameter. */
    unsigned char *dyn_type;
    /* AND THE ADDRESS ITS BYTES CAME FROM — HTML §8.1.4.2 "Fetching scripts": "let script be the result of
       creating a classic script given sourceText, settingsObject, RESPONSE'S URL, options, mutedErrors, and
       url". NULL for an INLINE row, whose base URL HTML §4.12.1.1 "Processing model" states as "el's node
       document's document base URL" and which the compile therefore reads from the document instead.
       IT CANNOT BE THE BODY COLUMN, because that column is where the address LIVED and the reply DESTROYS it: a
       DYN_SCRIPT_SRC row holds its URL in `dyn` only until flow_deliver_one_reply replaces it with the source text.
       Everything the address decides is needed after that moment — a nested `import('./chunk.js')` inside a
       bundle served from `/assets/app.js` resolves to `/assets/chunk.js`, and for a MODULE the address is
       additionally the module map KEY, so two `<script type=module src>` of one document named by their
       document rather than by themselves are ONE module and the second evaluates nothing. Parallel to
       `dyn_cand` for the reason stated there — the seven arrays are allocated, copied and freed together. */
    char **dyn_url;
    /* AND THE `script` ELEMENT THE ROW IS THE PROGRAM OF, or NULL for a row no element put there.
       HTML §4.12.1.1 "Processing model"'s "execute the script element" is a switch on EL, and its "classic" arm
       sets the document's §3.1.7 `currentScript` to that element for the whole of the run — so the element is a
       fact about the ROW, exactly as its type and its address are, and it has to travel with the row because
       the run is a WORK ITEM: the program starts in one scheduler step and finishes in another, with siblings
       running in between, so nothing at the completion could re-derive which element this was.
       NULL IS A POSITIVE STATEMENT AND NOT A HOLE — it is §3.1.7's own answer for a program with no `script`
       element behind it, which is most of them: a §8.6 string handler, a lazy chunk's reply, §7.4.2.3.2's
       `javascript:` URL, an @S candidate and a cross-agent operation's program are all classic scripts that no
       element caused, and a document running one of them has `currentScript` null.
       A BORROWED NODE POINTER, AND IT MAY NEVER CROSS A PARK. The cold tier stores a RECIPE and replays the
       document from its first script, so a resumed flow re-queues its rows and no pointer here outlives the
       session — which is exactly why the element can be a pointer at all. A snapshot FORK copies it, which is
       sound for the same reason the DOM base chain is: the sibling holds a reference on the segment the node
       was created in. Parallel to `dyn_cand` for the reason stated there — the arrays are allocated, copied
       and freed together. */
    lxb_dom_element_t **dyn_el;
    /* AND WHETHER EACH ROW IS A TASK OR THE SYNCHRONOUS TAIL OF THE PROGRAM THAT CAUSED IT (a DynPos,
       engine.h). The position a row was queued at is not consumed by the insertion — it is a fact the row
       KEEPS, because the MICROTASK CHECKPOINT is placed against it. HTML §8.1.4.4 "Calling scripts" performs
       the checkpoint when the JavaScript execution context stack empties, and §4.12.1.1 "Processing model"
       ends "prepare the script element" with "Otherwise, immediately execute the script element el, even if
       other scripts are already executing" — so a DYN_POS_IMMEDIATE row ran INSIDE the program that caused it,
       the stack never emptied across it, and the checkpoint that program owes falls AFTER the row. Every other
       row is a task and the checkpoint falls BEFORE it. Position alone cannot say which: an immediate row and
       an appended one both land at the cursor when the queue was empty, so without this column the two are the
       same row and one of the two orderings is silently wrong. Parallel to `dyn_cand` for the reason stated
       there — the seven arrays are allocated, copied and freed together. */
    unsigned char *dyn_pos;
    void *delta;           /* this flow's isolated HEAP COW delta (CowDelta*), applied while running */
    void *dom; int dom_n, dom_cap;   /* this flow's isolated DOM COW delta HEAD buffer (dom_cow), swapped with the
                                        heap delta on every context-switch so the DOCUMENT is a per-flow time-travel
                                        entity: two flows see different trees/attributes, a rewind restores the
                                        exact document the flow saw. Detached via dom_buf_take while parked. */
    void *dom_base;        /* the shared IMMUTABLE base-segment chain below the head (dom_cow_fork): a snapshot-
                              forked sibling references the parent's O(N) DOM delta in O(1). NULL until a fork. */
    void *dec_blob;        /* suspended decision state while paused (decide_suspend) */
    void *pin_blob;        /* suspended pin state while paused (concolic_pins_suspend) */
    /* ASYNC-AS-FLOW: this flow's OWN queued microtasks AND tasks, run under its live COW so a reaction runs in
       the timeline that enqueued it. A MICROTASK runs at the checkpoint HTML §8.1.4.4 "Calling scripts" owes
       once the program that queued it has left the stack — which is BEFORE the flow's next program, not after
       its last one — and a TASK runs when the sequence is exhausted. One array
       keeps both in a single arrival order — which is what a task source needs among its own tasks — and the
       pick (flow_job_take) applies the checkpoint rule.
       THE FUNCTION THIS NAMED FOR THE TASK RULE WAS flow_checkpoint_due, WHICH IS THE MICROTASK ARM. A reader
       following it landed on the predicate for the sentence one clause above and found nothing about tasks
       there at all; the task rule is the `else if (flow_job_pending(f) > 0)` arm of flow_step, and it is an
       `else` on the sequence test, which is where "when the sequence is exhausted" comes from.
       AND THAT SENTENCE READS AS A DESIGN AND STATES A DEFECT, which is why it now says where to go: a flow's
       sequence is a set the page's own programs EXTEND, so the condition is one page code can hold false and
       this queue's tasks are excluded while it does. It is not repairable by reordering those two arms —
       §8.1.7.1 "Definitions" requires each task source to be in ONE queue and the TIMER task source is in both
       of a flow's (a Function handler here through JS_EnqueueCallTask, a STRING handler in `dyn` through
       core/timing/timer.c's script sink), so the two queues partition by CARRIER where the spec partitions by
       SOURCE. The derivation, and what closes it, are at that arm in engine.c and are not restated here.
       A JS ARRAY OF IMMUTABLE JOB RECORDS, and it was the LAST malloc'd platform queue on a Flow — a
       `FlowJob *` grown by realloc, each entry holding a malloc'd `JSValue *argv` beside two raw pointers.
       The three reasons pending.h gives for the register below it are the same three here and every one of
       them was live: the runtime's leak walk cannot see a `JSValue *argv` (the values in it are GC objects the
       walk reaches only through a root it has, and a bare malloc is not one), a per-job malloc crossed neither
       a park nor a fork, and the FORK deep-copied every entry — a malloc per job plus a dup per argument —
       where an Array naming the parent's entries costs one refcount each. It also had FOUR hand-maintained
       sites (the enqueue, the fork's field-by-field copy, §7.5.10's drop and the release's free loop), the
       middle one of which carried a comment saying in as many words that a field added to the struct was an
       obligation nothing but that comment enforced. There is no struct left to add a field to.
       JS_UNDEFINED for a flow that has never been enqueued one, which keeps flow_job_pending a tag test —
       asked at every suspend point the interpreter offers, exactly as the register below it is. */
    JSValue jobs;
    /* FETCH-AWAIT: this flow's OWN live (pending) fetches and the synchronous requests it is blocked on,
       resolved when the flow's scripts+microtasks stall (the network completing). A JS ARRAY of plain records
       (solver/pending.h) rather than a malloc'd list, because CLAUDE.md §State-isolation says so in as many
       words: it must park to the cold tier, resume byte-identically and fork per-flow, and a `char *` does
       none of the three. JS_UNDEFINED for a flow that has never parked on anything, which is most of them. */
    JSValue pending;
    /* THE PARKED CONTINUATIONS, swapped with everything else on a context switch. A forced preempt inside
       job-driven code parks a suspended async activation on the runtime's pump queue; those activations belong
       to THIS flow's timeline and resume under THIS flow's delta, so leaving them in the runtime while a
       sibling runs would either resume them against the wrong heap or drop them outright. NULL for a flow with
       nothing parked, which is every flow that has not preempted inside a reaction.
       IT IS THE WHOLE SET AND ONE OPAQUE HANDLE, not four fields naming one park. A step reaches as many bases
       as it reaches — a settle nested inside a reaction parks two, and an async body completing while the
       reaction that resumed it is still on the C stack parks two — and each park's record lives on the base it
       suspends (quickjs's JSAsyncFunctionState), so what crosses the switch is their ORDER. Four fields could
       only ever carry one, which is why they are gone rather than multiplied.
       THE EXAMPLE THAT USED TO STAND HERE — "a host drain settling two fetch replies in a row parks two" — IS
       GONE BECAUSE THE DRAIN IS. flow_deliver_one_reply delivers ONE answered entry and the step ends, so two
       replies can no longer be settled inside one step and the second can no longer park behind the first;
       that walk was reordering the page's microtasks, and one-per-step is the fix. The FIFO is unaffected: it
       is required by the NESTED cases above, which a cadence cannot remove.
       IT IS A REFERENCE THE FLOW OWNS, not one it merely remembers: each continuation is kept alive by a
       reference its park took and only its resume gives back, so a flow that is torn down or PAGED OUT — where
       a recipe replays the work and frees none of the memory — gives them back itself, through
       JS_FreeParkedFlows at flow_release. */
    void *parked;
    /* THE ROUTED CROSS-DOCUMENT DELIVERIES THIS TIMELINE HAS BEEN HANDED AND NOT YET MADE — each the record
       the trusted zone routed here, paired with the SENDER's origin, which only that zone may stamp
       (SECURITY.md: an origin the untrusted engine computed for a foreign message is a forgery every
       `event.origin` check in every bundle would then trust). A delivery is a WORK ITEM ON THE ONE FRONTIER
       and this is how it is carried. It is attached to EVERY live flow of the receiving document, because a
       document's state IS its flows: the page's `message` listener was registered by a script, so it lives in
       the delta of the flow that ran it, and a delivery made anywhere else arrives at a document where nothing
       is listening. Each flow makes its own delivery when it next steps, under its own delta, and the task
       that step enqueues lands on that flow's own queue — which is why the queue is per-flow.
       IT IS A FIFO AND NOT A SLOT, for the reason the operation queue below it is one and for a stronger one:
       HTML §9.3.3 "Posting messages" ends the window post message steps by QUEUEING A GLOBAL TASK on the
       posted message task source, and §8.1.7.1 "Definitions" gives a task a SOURCE precisely to "group and
       serialize related tasks" — so two posts from one sender are two tasks the page observes IN ORDER, not
       two alternatives one of which may overwrite the other. A slot made the second post an abort, and it is
       on the shortest path there is: one sender posting twice. Only senders whose worlds CONTRADICT may not
       share a timeline, and that is a FORK rather than a queue entry — asserted at the arrival, where both
       vectors are in one hand (engine_route).
       A JS ARRAY OF IMMUTABLE [record, senderOrigin] PAIRS, for the three reasons pending.h gives for the
       register beside it and CLAUDE.md §State-isolation states in as many words: the runtime's leak walk
       cannot see a `char *`, a pair of raw pointers crosses neither a park nor an instance while text does,
       and a fork that shares the pairs by reference costs one refcount each instead of strdup'ing every byte.
       The pairs are what let this queue PARK: a flow holding one had no legal move under RAM pressure while it
       was two malloc'd strings the cold tier could not write (cold.c's park writes them as 'm' records now).
       JS_UNDEFINED on a flow with nothing to deliver, which is nearly all of them, so the common case stays a
       tag test. */
    JSValue deliver_q;
    /* WHICH SENDING TIMELINES THIS RECEIVING TIMELINE IS IN — the other half of what a routed delivery's world
       means, and the field that makes the queue above a QUEUE rather than a merge.
       A cross-document message carries the SENDING FLOW'S WORLD (CLAUDE.md §Security), and a delivery seeds
       work whose world is receiver-baseline ∧ that vector. Two arms of one sender branch carry two vectors
       that CONTRADICT (world_vec_relate), and a timeline that received both would be one neither sender was
       ever in. Which arm a timeline is in is therefore a fact ABOUT THE TIMELINE and has to be written down:
       without it the very same pair is refused when both are queued at once and accepted when the first is
       delivered before the second arrives, which is the schedule-dependent finding §Testing's differential
       exists to catch.
       EACH ENTRY IS AN IMMUTABLE [vector, taken] PAIR, and the flag is the whole of the mechanism:
         - taken = 1: a world this timeline RECEIVED. Nothing that CONTRADICTS it may be received here.
         - taken = 0: a world subtree this timeline FORECLOSED — the arm's half of a delivery-time fork. It is
           the sibling minted where its parent took `vector`, so nothing at or under `vector` is this
           timeline's; that message is the parent's and is delivered there.
       A JS ARRAY OF PAIRS for the three reasons the two queues above are: the runtime's leak walk cannot see a
       `char *`, TEXT crosses a park and an instance where a pointer does not, and a fork hands each arm its own
       Array naming the parent's entries at one refcount each. It is NOT a work item and is never on the list a
       release refuses to drop — it is this flow's IDENTITY as a receiver, so it is carried across the fork and
       the cold tier ('r' records) exactly as the path is. JS_UNDEFINED until the first cross-document message
       reaches this document, which is nearly every flow there has ever been. */
    JSValue deliver_world_q;
    /* A CROSS-AGENT OPERATION THIS INSTANCE WAS ASKED TO PERFORM — the record the asking instance wrote
       (core/frame/remote_op.h) and the trusted zone's rendezvous TOKEN for the flow that is waiting on it.
       IT IS THE SAME SHAPE AS THE DELIVERY ABOVE AND FOR THE SAME REASON, with one thing added: a delivery is
       one-way and this one owes an ANSWER. A document's state IS its flows, so `otherW.length` has N answers
       for N timelines — the record is attached to every live flow exactly as a delivery is, and each of them
       answers under its own delta. A channel that carried one answer would silently pick a timeline.
       IT IS A FIFO AND NOT A SLOT, because the operations are SEQUENTIAL rather than alternative: the asking
       side parks on each in turn, so a second one arriving before the first has started is an ordinary second
       question and not two contradictory askers (which would need a fork, not a queue). A slot made that an
       abort, and it is on the shortest path there is — route.mjs phase 3 withholds one answer and asks again,
       and the two records differ only in the asking WORLD.
       AN ENTRY IS CONSUMED WHEN THE FLOW TURNS IT INTO A PROGRAM: the record has nothing left to say and the
       token MOVES onto that program's row (`dyn_token` above), because from then on the question and the
       program are one thing. Which document the operation names is the row's too — `dyn_doc` already says where
       a queued program is compiled, and a second copy on the flow was a copy that could be behind.
       A JS ARRAY OF IMMUTABLE [record, token] PAIRS, for the three reasons pending.h gives for the register
       beside it: the runtime's leak walk cannot see a `char *`, a pair of raw pointers crosses neither a park
       nor an instance while text does, and a fork that shares the pairs by reference costs one refcount each
       instead of strdup'ing every byte. The ARRAY is per-flow and must be — each arm starts its own copy of a
       pending operation and each answers under its own delta, which is the multiplicity §7.2.1 has when a
       document's state is its flows. JS_UNDEFINED on a flow with nothing outstanding, which is nearly all of
       them, so the common case stays a tag test. */
    JSValue perform_q;
} Flow;

/* `doc_name` is THIS INSTANCE'S DOCUMENT identity, and it is a parameter rather than a separate init call so a
   frontier cannot exist without one: every flow is minted a world named by it, and two instances that shared a
   name would hand each other's flows the same segment. The host names the ROOT document, because the host is
   what knows there is more than one; every document below it is named by the one that created it. */
void  flow_registry_init(const char *doc_name);
void  flow_registry_free(JSContext *ctx);

/* Add a flow to the frontier, standing on nothing: an empty decision vector, which is what a from-baseline flow
   IS. A flow with a recorded path gets it by having its `dec_blob` installed after the add — by the fork that
   prepared it, or by the cold tier that rebuilt it — because that path is a reference on a SHARED chain and
   never an array this call could take ownership of. Dups `fn`. Returns the stored Flow* (stable until removed).
   Never fails (OOM aborts via CHECK — a dropped flow corrupts the frontier).
   The flow is SEEDED with this agent's root document's programs as rows of its own queue, because that is what
   a fresh timeline of this document IS — see flow_set_seed_hook. A creator whose flow must NOT start there
   (the fork, which inherits its parent's rows; a joined document's boot flow, which is seeded with that
   document's) says so by name through flow_add_unseeded. */
Flow *flow_add(JSContext *ctx, JSValueConst fn, WorldId parent);
Flow *flow_add_unseeded(JSContext *ctx, JSValueConst fn, WorldId parent);
/* WHAT A NEW FLOW'S PROGRAM SEQUENCE IS, answered by the component that owns the document's script inventory
   (solver/engine.c) and asked at the ONE place a flow is created. Installed when a session opens and removed
   when it closes, for the reason every other scheduler hook is: this layer owns the frontier and may not
   depend on what a document's programs are. */
void flow_set_seed_hook(void (*fn)(Flow *f));
/* How many flows this document ever created — the other half of the switch count. A run whose cost jumped needs
   to say WHICH grew: the frontier, or the work per flow. */
long flow_created_count(void);

/* IS THIS FLOW BLOCKED ON THE HOST? True while it holds an unanswered synchronous request. A blocked flow
   cannot make progress, so the preempt hook always yields it and a mid-frame yield reports it host-owed rather
   than runnable — otherwise the scheduler re-enters it immediately and it spins on an answer that cannot
   arrive while it holds the thread. */
int flow_blocked(const Flow *f);

/* IS THIS FLOW'S JAVASCRIPT EXECUTION CONTEXT STACK EMPTY? — HTML §8.1.4.4 "Calling scripts", clean up after
   running script step 3: "If the JavaScript execution context stack is now empty, perform a microtask
   checkpoint." That sentence is the precondition of a MICROTASK checkpoint and of a TASK alike, so it is what
   both the checkpoint arm and the reply-delivery arm of the scheduler's ladder are guarded on, and the full
   derivation of its two halves is at the definition in flow.c.
   IT IS EXPORTED BECAUSE IT HAS TWO CONSUMERS AND ONE OF THEM IS THE CENSUS. `framed` answers only the first
   half — a live frame — so the number of members that can take a task is NOT `flows - framed`, and a run that
   reads the second for the first cannot tell a frontier whose members are all mid-program from one whose
   delivery arm is unreachable for some other reason. Restated in the census it would be a second spelling of
   one spec sentence, which is the drift solver/pending.h refuses for the word "owed": what may differ between
   readers is what they DO, never what the question means. */
int flow_stack_empty(const Flow *f);

/* HOW MANY ROUTED CROSS-DOCUMENT DELIVERIES THIS FLOW IS HOLDING — the length of `deliver_q`, asked here
   rather than read at the call sites so the queue's shape has ONE reader (the twin below says what a second
   reader costs: it is always the one missing the tag assert). 0 for the JS_UNDEFINED an untouched flow
   carries, which is nearly all of them. */
int flow_deliver_pending(const Flow *f);

/* …AND THE THREE THINGS THAT EVER HAPPEN TO THAT QUEUE, WHICH LIVE HERE BECAUSE THE FIELD DOES. An entry is
 * never edited after it is pushed, so there are exactly three: APPEND at arrival, TAKE from the front at the
 * delivery, and a FORK that gives an arm its own Array naming the same entries. They are declared beside the
 * field rather than kept private to the scheduler because the queue has MORE THAN ONE client — engine.c
 * routes and delivers, cold.c writes it out and reads it back — and a second client writing the Array is the
 * two-readers-of-one-shape defect this file names one paragraph down: the second is always the one missing
 * the assert. Every mutation runs inside cow_engine_write_begin/end, because this is the SCHEDULER's record
 * about a flow, written from outside any flow's delta (engine_route walks every flow) — a delta that captured
 * it would put a delivered message back on the queue the moment a sibling switched in.
 * `flow_deliver_take` and `flow_deliver_entry` hand back an entry the CALLER owns and frees. */
void    flow_deliver_push(JSContext *ctx, Flow *f, const char *record, const char *sender_origin);
JSValue flow_deliver_take(JSContext *ctx, Flow *f);
JSValue flow_deliver_entry(const Flow *f, int i);
JSValue flow_deliver_fork(JSContext *ctx, const Flow *parent);

/* …AND THE SAME FOUR OVER THE COMMITMENT RECORD BESIDE IT (`deliver_world_q` above), which is a separate list
 * because it answers a different question and outlives every entry of the queue: the queue is what this
 * timeline still has to DO, this is what it has already BECOME.
 * The pair is [vector, taken] — the sending world's wire vector, and whether this timeline received it (1) or
 * foreclosed it (0). Entries are never edited after they are pushed, which is what lets a fork share them; the
 * ARRAY is per-flow because each arm adds its own commitments from the branch onward.
 * `flow_world_commit_at` hands back an entry the CALLER owns and frees, exactly as the delivery queue's does.
 * WHAT A COMMITMENT MEANS is engine.c's (it is the one component that holds both a record's vector and the
 * receiving flow); what lives here is the FIELD and the four things that happen to it, for the reason the
 * queue's four live here: a second writer of the Array is always the one missing the shape assert. */
int     flow_world_commits(const Flow *f);
JSValue flow_world_commit_at(const Flow *f, int i);
void    flow_world_commit_push(JSContext *ctx, Flow *f, const char *vector, int taken);
JSValue flow_world_commit_fork(JSContext *ctx, const Flow *parent);

/* THIS FLOW'S JOB QUEUE, AND EVERYTHING THAT EVER HAPPENS TO IT — declared beside the field for the reason the
 * delivery queue's four are: the queue has MORE THAN ONE client (engine.c enqueues, picks and drops; cold.c
 * counts) and a second client reading the Array's shape is always the one missing the assert. A record is
 * never edited after it is pushed, which is what lets a fork SHARE the entries and hand each arm its own Array.
 *
 * WHAT A RECORD IS, and why each part of it is there:
 *   - THE CALLEE, NAMED. A JSJobFunc is a static of the interpreter (promise_reaction_job,
 *     js_promise_resolve_thenable_job, js_dynamic_import_job, and host_call_job for every platform edge), so
 *     the host cannot enumerate them and names them by ARRIVAL instead — an ordinal into a table this file
 *     keeps. The point is not that the ordinal is portable (it is NOT: it is minted in arrival order within
 *     one session, which is the session-local-name defect cold.c refuses for a rendezvous token and for a
 *     WorldId generation), it is that NOTHING IN THE RECORD IS A POINTER. What a park would still have to
 *     write is then a NAME, and the reason it cannot is the next line rather than this one.
 *   - THE ARGUMENTS, as ordinary elements. This is what a park cannot carry and what makes the queue
 *     un-parkable today: a promise reaction's arguments ARE the reaction's capability functions, a delivered
 *     message's are the event init record holding a WindowProxy, and none of them has an identity outside this
 *     session — the same sentence cold.h writes about the pending register's `resolve`. A replay regenerates
 *     them for every job whose CAUSE is inside the replayed program, which is all of them except one; see
 *     cold_park_flow for the exception and for what closes it.
 *   - THE ENQUEUING REALM'S GLOBAL OBJECT, which is the key HTML §7.5.10 "Destroying documents"'s destroy a
 *     document step 7 uses: a task whose document has been destroyed is removed WITHOUT running. It is the
 *     realm's global rather than its JSContext* because the record must hold no pointer, and holding it as
 *     a REFERENCE also closes what the borrowed pointer left open — a realm freed without the drop hook
 *     running left every job of it holding a dangling key the next §7.5.10 walk would compare against.
 *   - WHETHER IT IS A TASK, which is not a label: the event loop performs a microtask checkpoint between one
 *     task and the next, so a task may not run while this flow still holds a microtask.
 *   - ITS HANDLE — the name the RUNTIME issued at the enqueue (quickjs.h's JSTaskHandle), which is the only
 *     thing that can find this record again. HTML's toggle task trackers (§4.11.4 "The dialog element" step 1
 *     of queue a dialog toggle event task: "Remove element's dialog toggle task tracker's task from its task
 *     queue"; §4.11.1 "The details element" holds the same tracker) coalesce N transitions in one turn into
 *     ONE event by taking the still-queued task back off the queue, and a queue whose entries have no identity
 *     cannot obey that sentence. HELD AS A JS NUMBER, which is exact for every handle a runtime can issue:
 *     js_task_handle_new DCHECKs the monotone counter never reaches 2^53 precisely because the OTHER end of
 *     this name is an element's tracker slot, and a slot is a JS value. So the record and the tracker speak
 *     one representation with no conversion between them that could round. The alternatives were weighed and
 *     are worse for reasons that are not taste: a STRING allocates at every push and compares by content at
 *     every walk, for a value that is an integer; a TWO-WORD PAIR adds a split/join rule to every reader of
 *     the record to cover a range the runtime asserts is unreachable — a rule with no reachable input is a
 *     rule nothing can exercise. JS_TASK_HANDLE_NONE is the never-issued value and is what a record pushed by
 *     something other than the runtime's enqueue path honestly carries; it names nothing, so nothing finds it.
 *   - WHERE THE WORK CAME FROM. A job the replaying program causes is regenerated by the replay; a job caused
 *     by something OUTSIDE it is not, and the only such job in this engine is the one a routed cross-document
 *     delivery becomes (engine.c's flow_deliver, which brackets the conversion with flow_job_external_begin /
 *     _end). Recorded at the push, so an entry stays immutable, and read only by the park.
 *
 * Every mutation runs inside cow_engine_write_begin/end, for pending.h's reason exactly: this is the
 * SCHEDULER's record about a flow rather than state a page wrote, and it is mutated from inside whichever
 * flow's delta happens to be applied — a routed delivery and engine_unload_document's fan-out both push onto
 * a queue that is NOT the running flow's. A delta that captured any of it would put a dropped job back on the
 * queue, or take a pushed one off it, the moment a sibling switched in. */
int  flow_job_pending(const Flow *f);
/* DOES IT STILL HOLD A MICROTASK? The checkpoint is over exactly when it does not — a task on the queue is the
   NEXT turn of the event loop and not part of this checkpoint, which is the same distinction the pick makes. */
int  flow_job_microtask(const Flow *f);
/* APPEND, with `argv` dup'd into the record. `task` picks which of HTML §8.1.7 "Event loops"' two queues.
   `handle` is the name the runtime issued for this callback (see the record's HANDLE bullet above), carried
   into the record because the host that TOOK the job is then the only thing that can find it again. */
void flow_job_push(JSContext *ctx, Flow *f, JSJobFunc *fn, int argc, JSValueConst *argv, int task,
                   JSTaskHandle handle);
/* THE PICK, WHICH IS HTML §8.1.7.3 "Processing model"'s MICROTASK CHECKPOINT AND NOT A FIFO POP — the
   oldest microtask if there is one, else the oldest task. Removed from the queue and returned OWNED; a plain
   FIFO ran `setTimeout(f, 0)` in the middle of a promise chain, which is the one ordering the event loop
   exists to forbid. */
JSValue flow_job_take(JSContext *ctx, Flow *f);
/* RUN ONE — the callee called with its own arguments. The record's shape has exactly one reader and this is
   it, so a caller holds an opaque entry and never an argument vector. Returns the callee's result, owned. */
JSValue flow_job_run(JSContext *ctx, JSValueConst entry);
/* HTML §7.5.10 "Destroying documents", destroy a document step 7, for ONE flow: remove every job whose
   enqueuing realm is `realm`, WITHOUT running it.
   Returns how many went. */
int  flow_job_drop_realm(JSContext *ctx, Flow *f, JSContext *realm);
/* THE OTHER REMOVAL — BY NAME rather than by document, for ONE flow: take the job called `handle` off this
   flow's queue WITHOUT running it, and answer whether one was there (0 or 1). This is quickjs.h's
   JSJobRemoveHook for a host that took ownership of the job, and the one it exists for is HTML §4.11.4 "The
   dialog element"'s "Remove element's dialog toggle task tracker's task from its task queue".
   FINDING NOTHING IS AN ORDINARY ANSWER and the reason the name is a monotone integer nobody re-issues: the
   task may already have run, may be the one running right now, or may have gone with its document — a handle
   outlives what it names, so 0 is what "already run" means and is never an error.
   IT IS ASKED OF ONE FLOW, AND THAT IS A DESIGN STATEMENT rather than a convenience — see the caller in
   engine.c: a fork gives each arm its own Array naming the SAME records, so after a branch two flows hold two
   queued copies of one handle. Each arm's tracker names the copy in its own timeline. */
int  flow_job_remove(Flow *f, JSTaskHandle handle);
/* THE ARM'S OWN QUEUE — a new Array naming the parent's RECORDS. The array is per-flow (each arm runs its jobs
   at its own rate under its own delta); the records are shared, because none of them ever changes. */
JSValue flow_job_fork(JSContext *ctx, const Flow *parent);
/* THE WORK THIS FLOW'S QUEUE HOLDS THAT A REPLAY WOULD NOT RE-CAUSE — see the record's last bullet. Read by
   the park, which may not write it down and may not silently drop it either. */
int  flow_job_external(const Flow *f);
/* THE BRACKET THAT MARKS IT. Everything enqueued between these two calls came from outside the replayed
   program. Nests nowhere: a delivery is made from flow_step with no frame, so no second conversion can be in
   flight, and that is asserted rather than counted. */
void flow_job_external_begin(void);
void flow_job_external_end(void);

/* HOW MANY CROSS-AGENT OPERATIONS THIS FLOW HAS BEEN ASKED AND NOT YET STARTED — the length of `perform_q`,
   asked here rather than read at the call sites so the queue's shape has one reader. 0 for the JS_UNDEFINED an
   untouched flow carries, which is nearly all of them, so the scheduler's pick stays a tag test. */
int flow_perform_pending(const Flow *f);

/* DOES THIS FLOW STILL OWE A PEER AN ANSWER — asked of the QUEUE and of the PROGRAM ROWS together, because an
   operation is in one or the other from the moment it arrives until its program's completion is sent. Both
   halves are load-bearing and neither alone is the invariant: an entry still queued is a question nobody has
   performed, and a row still holding a token is a question performed and never answered. Every site that ends
   a flow reads this, because a token that dies with the flow is a flow in ANOTHER instance suspended at the
   line that asked, forever, and nothing on this side would ever say so. */
int flow_owes_answer(const Flow *f);

/* The WFQ priority of a flow (higher = run sooner). Pure function of the flow's reward/aging/visit state. */
double flow_weight(const Flow *f);

/* §scheduler'S REWARD TERM AS THE ORDER READS IT — this flow's FORK FAMILY's accumulated emitted value, not the
 * member's own `val`, which ranks nothing (see the field). It is exported because two consumers outside flow.c
 * have to name the quantity flow_weight is actually a function of and the family node is private to that file:
 * the scheduler's ranked-state cache, which asserts that the value yield only fires when a term of the weight
 * MOVED — a cache keyed on the member's own ledger would go on agreeing while the rank changed underneath it —
 * and the cold tier, whose recipe carries (path, reward) across a session and must carry the reward the
 * resumed frontier will be ordered by. Departed flows read 0.0: their account is gone and so is their rank. */
double flow_reward(const Flow *f);

/* …AND THE ONE WRITER OF IT THAT IS NEITHER AN EMISSION NOR AN ARRIVAL — the cold tier's rebuild, REPLACING
 * the account a from-baseline flow was placed at with the one the session that parked it wrote down. A resumed
 * member is not a newcomer, so it does not enter at the frontier's virtual time; it comes back at its own.
 * It is not exported for anyone else: a caller that wanted to SET a reward wants flow_credit_emit, which is a
 * ledger entry and is paid once per observation. See flow.c. */
void flow_restore_reward(Flow *f, double val);

/* THIS FLOW COMPLETED A UNIT OF WORK — the optimism term's "visit", credited by the scheduler at the ONE point
 * that can see the whole of a step: after flow_step returns, when the flow is left BETWEEN units. It is a
 * scheduler statement rather than a flow_step one because flow_step returns from a dozen arms and half of them
 * leave the flow suspended in the middle of a program, which is not a completed trial; the caller already
 * computes that predicate for HTML §8.1.7.3 "Processing model"'s end-of-checkpoint steps and this is the same
 * boundary. Asserted at its site: a flow inside a program may not be credited one, because the whole reason the
 * term is a unit count is that thread time inside a program is exactly what it must NOT measure. */
void flow_credit_visit(Flow *f);

/* THE SCHEDULER HANDED THIS MEMBER THE THREAD — the one writer of `picks` (see the field for why a DISPATCH
 * count is the only statement about a member that an emission cannot erase). It is credited at the single
 * point every dispatch converges on, the context switch itself, so a new call shape cannot forget to route:
 * there is no route to remember. A member that keeps the thread across consecutive steps is credited ONCE,
 * which is what the row is about — `picks == 0` is "never chosen", not "not chosen lately". */
void flow_credit_pick(Flow *f);

/* WHAT THE ORDERING IS MADE OF — the census that turns "the WFQ's value ordering" from a claim into a number.
 *
 * The engine already reports how much work is happening (@PROGRESS's switches/forks) and how much of it
 * RETIRES (@COLD's `finished`), and a run in which both climb while the fixture's own probe table stops
 * advancing is a run in which every member of the frontier is doing something that emits nothing. Neither
 * stream can say WHY, because neither reads either term the pick is made of, and the shape of the question is
 * specific: flow_weight is reward + optimism − aging, and THE OPTIMISM TERM'S ENTIRE RANGE IS 1.0 — one
 * emission. A member whose reward is a point below another's is therefore outranked no matter how long it has
 * waited, and it is reached only by the aging term, which gives back FLOW_AGE_RATE per microsecond the other
 * flow AND its chain burn without emitting. The reward SPREAD over the frontier is the whole of whether the
 * optimism term can still order anything, and nothing measured it.
 *
 * `val_zero` NAMES A POPULATION THE ARRIVAL RULE DELETED, AND THE ROW IS KEPT PRECISELY TO SAY SO. It used to
 * be the from-baseline population — a candidate session, a joined document's boot flow, the first flow — on
 * the reasoning that such a flow "enters at reward 0 (flow_add's zeros), so its weight is at most 1.0 for its
 * whole life". That stopped being true the moment flow.c's arrival rule stopped writing a zero there, and the
 * mechanism has since been corrected once more in a way this row must not be read against the old version of:
 * the arrival ASSIGNS NOTHING AT ALL now. A from-baseline flow founds an account that is UNPLACED, and an
 * unplaced account's reward IS the frontier's virtual time (flow.c's FlowAcct `placed`), re-read at every
 * pick until the dispatch that first gives it the thread freezes it. So such a flow does not merely enter at
 * the incumbent's reward — it STAYS at the frontier's clock until it is served, which is the difference
 * between a one-time copy and a relation and is why this row cannot be non-zero for a member the order is
 * holding. So inside a
 * busy period this row is 0 BY CONSTRUCTION — it can be non-zero only before the first pick, where SFQ's v(0)
 * is 0 — and a reader that tests it for "is there a population down at the bottom" is testing for a set the
 * arrival rule made empty. That is not a hypothetical reading. The pairing this file cites the other way round
 * one screen up (`valZero:12` beside `cands:12`, the twelve candidate sessions that were never picked) reads
 * `valZero:0` beside `cands:2997` on that same fixture now, with the reward SPREAD at 168 points and every
 * other term's range at or below 1.0 — so the one discriminator that could have named the reward term as the
 * order was permanently false on the run where it was the answer.
 *
 * THE POPULATION IS `members - self_emit` INSTEAD, and the two rows are not interchangeable. That difference
 * is how many members have earned NONE of the reward they are ranked on, and it is what a reader wants when it
 * asks WHOSE reward the order is. A candidate is in that set for the reason it used to be in `val_zero`'s: it
 * records no endpoints by design (endpoint_suppress), so the only thing that can raise its account above what
 * it was placed at is the very sink it is trying to reach.
 * AND IT MEANS SOMETHING DIFFERENT NOW THAT THE REWARD IS THE FORK FAMILY'S, WHICH IS WORTH SAYING BECAUSE THE
 * ROW READS THE SAME. It used to say how much of the order was INHERITANCE — every arm holding a copy of a
 * prefix nobody standing there had earned — and a high count was the ordering defect itself. Held on the
 * account, the reward is no longer copied to anybody, so a member that has emitted nothing is not carrying a
 * rank it did not earn; it is standing in a family that did. The count is then a fact about the DOCUMENT's
 * branching rather than about the scheduler: how many live arms one producing account is spread over. What
 * makes it a finding again is the pairing — `members - self_emit` at the whole frontier WITH `families > 1`
 * and a reward spread, which is one account outranking another while none of the second's members can act.
 *
 * AND IT REPORTS THE WEIGHTS THEMSELVES, which is a reversal of what this comment used to say. It said the
 * census "calls flow_weight for nothing at all — it reports the two terms, never their sum", on the reasoning
 * that the terms are what a rank is MADE of. The terms are; the ORDER is the sum, and leaving the sum out
 * meant every reading of this census had to re-derive flow_weight by hand from `val` and a service notch. That
 * is not a theoretical cost: a reading of `valMax - valMin > 1` was taken here as "the reward term is ordering
 * the frontier" on a run whose `valTop` was 0.0 — the spread existed and was not what the pick read, and the
 * arithmetic that would have shown it (a val=3 member at 448 notches is at weight -2.37, below a from-baseline
 * flow at 1.0) was done by hand, three exchanges later, by someone who already had both terms in front of them.
 * The sum the pick actually uses is one call per member and it removes the re-derivation entirely.
 *
 * PURE MEASUREMENT: one scan, no reference taken, nothing mutated — safe between scheduler steps, which is
 * where the progress stream asks it. It decides NOTHING; every member keeps its weight and its place. */
typedef struct {
    long members;      /* live members of the frontier — the denominator for every count below */
    /* THE REWARD TERM'S RANGE OVER THE FRONTIER. A spread above 1.0 is the statement that the optimism bonus
       can no longer reorder its ends — its entire range is one emission, and it must stay that way or a
       PROMISE outweighs a FINDING (flow.c's flow_distance says the same of the fitness comparator).
       AND AGING DOES NOT "REACH THE BOTTOM", WHICH IS WHAT THIS ROW USED TO SAY. The aging is a charge for
       thread time CONSUMED, so it is subtracted from the flow being SERVED and from the family serving it: it
       cannot lift a member that consumes nothing, and the bottom of a frontier is made of exactly those. The
       ends therefore meet only by the TOP coming down, which is a derived quantity and not a property of the
       term — `(val_top - val_min + 1) / FLOW_AGE_QUANTUM` quanta of silence, counted as the top's own service
       plus its FAMILY's since any arm of that family last emitted (flow_silence_notch), and reset to zero by
       every one of those emissions (flow_credit_emit). So on a frontier whose leading family is still
       emitting there is no spread at which the two ends converge, and reading this row as "aging will get
       there eventually" is reading a term that only ever pushes the tail further away. WHAT THE ORDER IS
       ACTUALLY MADE OF is then this spread against `self_emit`: flow.c's flow_nonreward BOUNDS every term of
       the order except the reward, so the reward gap is the only quantity that can put a never-run member
       behind the flow the pick returns, and `self_emit` says whether that gap is something the members earned
       or something they were handed. The bound is asked of ONE member rather than of a pair — flow_pick's
       comment says why the pair form could not be asked at all once a frontier stopped emitting — so this row
       is read against a claim that holds on every frontier and not only on a freshly productive one.
       IT IS THE FORK FAMILY'S REWARD, READ PER MEMBER, AND THAT IS WHAT THE SPREAD NOW MEANS. The reward is
       held on the account the aging is charged to (flow.c's FlowAcct `val`), so every arm of one family reads
       one number and a frontier that is ONE family — which a real page's is, every flow descending from the
       boot flow — has a spread of exactly ZERO here whatever it does. That is not the row going blind; it is
       the row saying that within a family the guarantee flow_pick's third assertion is tight in holds
       outright, because the gap it is tight in is identically zero. A NON-ZERO spread is therefore a statement
       about several ACCOUNTS — several documents, several @S searches, a resumed frontier's rebuilt recipes —
       and is read beside `families` below, which says how many there are. Read as a per-member reward it was
       something else entirely: a per-CHAIN prefix, differing between arms by what their common parent emitted
       between their two branches, unbounded against every other term's range of 1.0, and therefore the whole
       of the order on exactly the frontier where it named nothing anybody had done. */
    double val_min;
    double val_max;
    double val_top;    /* …and flow_best's family's, so the top of the order is named rather than inferred */
    /* THE FRONTIER'S VIRTUAL TIME AT THE INSTANT OF THE CENSUS — flow.c's `g_vt`, SFQ's v(t), which is the
       queue coordinate of the item in service and therefore the coordinate every account that has never been
       served is standing at. It is NOT a reading of the walk and is assigned unconditionally, like
       `nonreward_max` beside it, so an empty frontier reports the clock rather than a zero that would read as
       one.
       IT IS THE SUBJECT `val_min` HAS ALWAYS BEEN MISSING. A pinned floor is a statement about the clock or
       about the members, and which one it is cannot be recovered from the floor alone: `val_min` far below
       `vt` says accounts are being LEFT BEHIND by a clock that has moved on; `val_min` tracking `vt` says the
       floor is where the queue actually stands. The pair is also the cheapest tell that this quantity has
       stopped being a clock at all — `vt` above `val_max` is a clock leading a frontier no member is standing
       at, and `vt` frozen while `val_max` climbs is the one-time copy this row was added to end. */
    double vt;
    /* MEMBERS WHOSE FORK FAMILY'S WHOLE REWARD IS ZERO — the population whose weight ceiling is 1.0, which is
       an ARITHMETIC fact about the reward and is what this row is for. It used to be described as "has emitted
       nothing" as well, and those were one population until a from-baseline flow started being PLACED at the
       frontier's virtual time: an arrived account has emitted nothing and holds whatever the leader held, so it
       is outside this row and inside the next one. Two questions, one bit, and the reward-band verdict needs
       the other one — see flow.c's FlowAcct for the split that separates them. */
    long val_zero;
    /* …AND THE POPULATION THAT ACTUALLY EMITTED NOTHING, which no row could name while the reward was one
       field. It is `earned == 0` at family scope: members standing entirely on the coordinate their account
       ARRIVED at, having produced nothing of their own since. That is the @S candidate session, the joined
       document's boot flow and the cold-resumed recipe — every from-baseline door — and it is exactly the
       population a pinned `val_min` beside a climbing `val_max` is a statement about. Read it with those two:
       a large count here whose accounts sit at the FLOOR of the reward band is the arrival coordinate being
       left behind by accounts that earn past it. THAT SENTENCE USED TO END "and no term of the order
       re-relates it", and `val_unplaced` below is the row that says whether it still applies to a given
       reading: the arrival is a CONTINUING relation now (flow.c's FlowAcct `placed`), so an account is held at
       the clock until it is first served and can be left behind only AFTER that. A large `val_arrived` at the
       floor with `val_unplaced` at zero is accounts that have been served and out-earned — the bandit working;
       the same reading with `val_unplaced` large is the relation itself having stopped working, which is a
       different defect and a different fix. `self_emit` beside it is the same question asked of ONE MEMBER
       rather than of its account, and the pair separates a family coasting on an ancestor's findings from a
       family that has none. */
    long val_arrived;
    /* …AND THE SUBSET OF THAT POPULATION WHOSE COORDINATE IS STILL A READING OF THE FRONTIER'S CLOCK RATHER
       THAN A TAG OF ITS OWN — accounts that have never once held the thread (flow.c's FlowAcct `placed`). The
       two rows are one question asked before and after the only event that changes the answer, and the pair is
       what makes `val_min` readable: an account is unplaced exactly while its reward IS `vt`, so a `val_min`
       far below `vt` with a large count HERE is the ordering failing to reach members it is holding at the
       clock, while the same `val_min` with this row at zero is a frontier of accounts that have all been
       served and have simply been out-earned. Those are opposite work — the first is a placement defect and
       the second is the bandit doing its job — and no single row can tell them apart.
       `val_arrived - val_unplaced` IS THE OTHER HALF AND IS THE MORE INTERESTING ONE ONCE THE FIRST IS ZERO:
       accounts that HAVE been given the thread and have emitted nothing with it, which is the population §@S
       means by "a near-miss is mutated toward the gap; a dead candidate starves". */
    long val_unplaced;
    long self_emit;    /* members with val > 0: they emitted something THEMSELVES rather than standing on an
                          account an ancestor filled. Zero here while `finished` climbs is work that advances no
                          statement. It is a plain test and no longer a subtraction, because nothing is
                          inherited to subtract. */
    long unrun;        /* members standing at ZERO OWN SILENCE (flow.c's flow_own_silence) — never charged for
                          the thread since their fork family last emitted. It is that reading and NOT the raw
                          `Flow.cpu`, and the difference is what this row was measured being wrong about: read
                          off the field it was nonzero at exactly TWO of 71 censuses — both while the frontier
                          was under a thousand members — and ZERO for every one after that, on a run reaching
                          3480, because a fork copies the parent's burn and nothing but a dispatch ever
                          cleared it. It counts a member of a family that has just PRODUCED as one that has
                          never run, which is why `never_picked` below exists.
                          IT IS NOT flow_pick'S OWN DEFINITION, which is what this row used to claim: the
                          pick's `unrun` is the population whose weight is at least 1.0, and that needs every
                          term of the weight at zero (visit count, own service, family service), not one of
                          them. This is the broader set on purpose — read against `vis_max` below it separates
                          "nothing has been charged yet" from "nothing has FINISHED anything", which is the
                          distinction the pair exists to make — but a reader taking it for the assert's
                          population would be reading a superset. */
    /* MEMBERS THE SCHEDULER HAS NEVER HANDED THE THREAD, AND HOW FAR THE BEST OF THEM STANDS BEHIND THE FRONT.
       §scheduler's razor forbids a resume that "drops, starves, skips, reorders, or forgets ANY flow", and
       STARVES is the only one of the five with no row anywhere in this struct — every other candidate (`unrun`,
       `vis_zero`, `svc_min`, flow_pick's own `unrun`) is a term of the weight, and flow_credit_emit resets the
       SILENCE ones for the whole emitting family, so each of those counts a member that has just PRODUCED
       something — and every arm standing beside it — as one that has never run. `vis_zero` is the exception
       since the emitter's per-member visit zero went, and it still cannot answer this: a member that finished
       nothing may have been dispatched into a program that never ends, which is a resume-seam defect wearing
       the same row. `picks == 0` is
       the population, and it cannot be moved by anything the member did, because it did nothing.
       THE PAIR IS THE READING AND NEITHER HALF IS ONE ALONE, which is the same shape `jobs_ready`/`job_w_gap`
       already takes one row down. The COUNT says such members exist; only the GAP — in the order's own points,
       against the weight flow_pick actually returned — says whether the ordering is what is keeping them out.
       A large gap with a large count is the WFQ working: those members are genuinely outranked and the aging
       term is the thing that will reach them.
       AND A GAP AT ZERO IS NOT THE OPPOSITE VERDICT — THIS BLOCK SAID IT WAS, AND THE SENTENCE IS RETIRED HERE
       RATHER THAN DELETED, because the reading it licensed is one a reader re-derives. It said a gap at or near
       ZERO beside a non-zero count was the razor's own state: members standing at the front of the order that
       the pick is nonetheless not returning, starvation rather than ordering, a defect in the DISPATCH rather
       than in the weight. It does not follow, and the refutation is in flow_pick. The comparison there is
       STRICT and the incumbent is the SEED, so on a frontier carrying a large EQUAL-WEIGHT cohort — the
       ordinary state of a one-family page, since every member of a family reads one reward through one pointer
       and an emission zeroes that family's silence at every arm at once — the pick returns ONE of N tied maxima
       and the other N-1 stand, at that instant, exactly at the front with `picks == 0`. Zero is then the
       EXPECTED reading of a healthy sweep, and this row cannot tell that state from the one the retired
       sentence named. solver/result.c carries the measurement that retired it and the retirement did not reach
       this block, which is how the stale sentence went on being quoted from here as the tree's standing verdict:
       six runs of the native fixture, 212 censuses, sixty-three samples at exactly 0.000, spread across every
       run including ones whose ladder drained all the way to the orphan seed.
       WHAT ENDS THE TIE IS NOT A DISPATCH RULE AND MUST NOT BECOME ONE, which is the half a reader reaching for
       a non-strict comparison here has already skipped. Relaxing flow_pick to `>=` would not implement
       §scheduler's "a never-run flow is never starved" — nothing is ranked AHEAD of a tied member, so there is
       no starvation for it to cure — it would hand the thread away on equality at every opcode, which is the
       switch-per-opcode the seed exists to stop. The incumbent's hold ends because it is STRICTLY DEMOTED, and
       two independent writers do that unconditionally: flow_credit_visit advances `visits` at the end of every
       completed unit of work, dropping the optimism term from 1/(1+v) to 1/(2+v); and flow_age_running charges
       the running flow's OWN silence, which advances flow_silence_notch and drops the weight by
       FLOW_AGE_QUANTUM for a flow that completes no unit at all. The FAMILY half of that charge lands on every
       arm through one pointer and cancels out of this gap (see `top_svc_fam` below); the OWN half is charged to
       the dispatched flow alone, so it is exactly the term that breaks the tie in the waiting member's favour.
       SO THE FINDING IS IN THE SERIES AND NOWHERE ELSE, and it is a THROUGHPUT statement rather than an ordering
       one: `never_picked` climbing across consecutive censuses while `members` grows is the tail not being
       reached. A single sample of this row — of any row here — characterises an instant and never a run. Left at
       0.0 when the population is empty, so a reader takes the two together (build.mjs reads the series). */
    long never_picked;
    double never_picked_gap;
    /* HOW MANY OF THEM STAND AT EXACTLY THE FRONT — the N of the retired sentence above, which that sentence
       NAMES and this struct could not supply. The refutation it carries is that a gap of 0.0 is the EXPECTED
       reading of a healthy sweep, because the pick returns "ONE of N tied maxima and the other N-1 stand, at
       that instant, exactly at the front with `picks == 0`"; with N unreported, a plateau of three and a
       plateau of six hundred are the same two digits on the same line. Those are not shades of one state.
       Three is a sweep in progress. Six hundred is an order whose within-family terms have stopped separating
       anything, so what returns the pick is the member's POSITION in flow.c's `g_flows` array and the weight
       is deciding nothing at all — and §scheduler's one WFQ is then a comparator standing beside the ordering
       rather than being it.
       IT IS A COUNT AND THE ROW ABOVE IT IS A DISTANCE, WHICH IS WHY NEITHER SUBSTITUTES FOR THE OTHER.
       `never_picked_gap` is a reading of ONE member — the most-favoured starved one — so it is silent about
       how many stand with it, and it is silent in exactly the direction that decides the repair: the best of a
       six-hundred-wide plateau and the best of a lone near-miss both read 0.0. This is that plateau's width,
       restricted to the population §scheduler's never-starved sentence is actually about.
       WHAT THE TWO WITHIN-FAMILY TERMS ARE, so a reader can price the number rather than only rank it. On a
       ONE-FAMILY frontier — every flow descending from the boot flow, which flow.c's `FlowAcct.val` calls the
       ordinary case — the reward and the FAMILY half of the aging are common offsets that cancel out of every
       comparison between members, so members are separated by the optimism bonus and their OWN silence and by
       nothing else. The bonus is 1/(1+`visits`), and `visits` is raised only by flow_credit_visit, which
       asserts `frame == NULL`: a member INSIDE a program cannot advance it, and a fork copies its parent's, so
       a chain of framed arms reads ONE bonus for all of them. The own silence is forgiven for the whole
       account at any arm's emission (flow.c's `FlowAcct.emit_gen`), so every member the thread has not been
       handed since that emission reads ZERO of it. This row is how many members those two facts have left the
       order unable to tell apart, and read beside `vis_zero` and `jobs_framed` it says which of the two did it.
       A GAUGE, NEVER DIFFERENCED, exactly like `never_picked` beside it and for its reason: a member that is
       chosen leaves this population and every member born since joins it, so the series falls as well as
       rises. `never_picked_at_top <= never_picked` by construction, and it is non-zero exactly when
       `never_picked_gap` is 0.0 — both asserted at the end of flow_wfq_census, which is what gives this row a
       reader in every dev build and what stops the pair coming to be about two different sets. */
    long never_picked_at_top;
    /* HOW THE DISPATCHES THAT DID HAPPEN WERE DISTRIBUTED, WHICH IS THE OTHER HALF OF THE PAIR ABOVE AND TAKES
       OPPOSITE WORK FROM IT. `never_picked` says a tail exists and `never_picked_gap` says how far behind it
       stands, and that pair has ONE answer for THREE states of the scheduler — states whose repairs are
       different and two of which are not repairs at all. Write M for `members`, P for `members - never_picked`
       (the live members ever chosen) and T for `picks_live` (the dispatches those members are still holding):
         T/P ≈ 1                      the thread reached a FRESH member nearly every time. The frontier is
                                      growing faster than one thread serves it, and that is a THROUGHPUT fact
                                      about branching and slice length; no term of flow_weight reaches it, and
                                      a weight change made against this reading fixes nothing and can only make
                                      the order worse.
         T/P ≫ 1, picks_max ≈ T/P     a reachable COHORT is being swept repeatedly while the tail waits: the
                                      order is returning members it has already served ahead of members it has
                                      never served. That is an ORDERING defect and it is the one the weight
                                      owns.
         picks_max ≈ T                ONE member is holding the thread and the switches are it and a single
                                      rival trading. That is a MONOPOLIZER the aging term is failing to sink —
                                      §scheduler's own sentence, and a third repair again.
       AND THE ROW ABOVE IS A GAUGE THAT MOVES, WHICH IS WHAT MAKES THESE NECESSARY RATHER THAN MERELY FULLER.
       `never_picked` counts the members standing NOW that have never been chosen, and a member that is chosen
       leaves that population while every member born since joins it — so the number falls as well as rises, and
       a single reading of it is a fact about one instant that says nothing about a run. Measured on one frozen
       build's own @WFQ series, in order: 0, 25, 218, 18, 37, 261, 445, while `members` went 1, 153, 347, 369,
       388, 612, 802. The 218 → 18 step is roughly two hundred members handed the thread between two samples, on
       a frontier that had grown by twenty-two — so "the tail is not being reached" was FALSE across that
       interval and true-looking at both ends of it, and the series that flow.h prescribes as the honest reading
       cannot be differenced either, because differencing a gauge is arithmetic over no quantity. The dispatches
       are what happened; only a counter can say how many there were.
       THE PAIR OF KINDS IS THE POINT AND IT IS SPELLED IN THE NAMES. `picks_live` and `picks_max` are GAUGES:
       they are taken over the members standing NOW, so a member that departs takes its dispatches out of both
       and either may FALL between two censuses. Differencing them is arithmetic over no quantity — the defect
       CLAUDE.md records as a gauge read as a lifetime histogram, whose free tell is a series that decreases.
       `picks_lifetime` is the LIFETIME COUNTER: every dispatch this instance has ever made, held off the flows
       entirely (flow.c's `g_picks_total`) because a per-member field cannot be one, and reset by nothing for
       the reason `rank_changes` is reset by nothing. It is the ONLY one of the three a reader may difference.
       THE IDENTITY THAT DEFINES THEM, and it is checked in both directions rather than described: within the
       census, `picks_live <= picks_lifetime` with the difference being exactly what departed members took away
       (asserted at the end of flow_wfq_census); across the document, `picks_lifetime` must EQUAL the result's
       `_switches`, because flow_credit_pick has exactly one caller and engine.c raises its own switch count on
       the line beside it. A reader who cannot check a counter's identity is holding a digit, not a
       measurement. */
    int64_t picks_live;       /* GAUGE: dispatches held by the members standing now */
    int64_t picks_max;        /* GAUGE: the most any one of them holds */
    int64_t picks_lifetime;   /* LIFETIME: every dispatch this instance has made, departed members included */
    int64_t svc_max;   /* the largest service notch in the frontier — who is actually consuming the thread */
    /* …AND THE OTHER END OF IT, WHICH IS THE ONLY NUMBER IN THIS STRUCT THAT CAN ANSWER "IS THE AGING TERM
       MEASURING THIS FLOW OR THE WHOLE FRONTIER". `svc_max` alone reads identically for a single monopolizer on
       an otherwise-idle frontier and for a frontier every member of which has burned the same thread time, and
       those take opposite actions: the first is the case the aging term was priced for, the second is one where
       every member is being charged for work the frontier as a whole did. The FLOOR is what separates them, and
       `svc_max - svc_min` is the SPREAD of service the ranking is actually made of. This clause used to add
       that the floor "is also the virtual time a from-baseline flow now arrives at", and that is retired: the
       arrival copies NO silence at all (flow.c's flow_arrive_at_virtual_time), because frontier_vt() is the
       serving item's whole queue coordinate with its aging already in it and copying the aging again would
       charge a newcomer twice for thread time it never consumed. A from-baseline family is born at zero
       service, so it enters this spread at the FLOOR by construction rather than at whatever the incumbent
       had burned.
       THE THREE WEIGHTS BELOW CANNOT ANSWER IT, which is a correction to what this file claimed. It said of
       `w_top`/`w_min`/`cand_w_max` that "these three numbers are what will show that" — and they cannot, by the
       arithmetic of the function they are readings of: raise EVERY member's service by the same amount and all
       three fall by exactly `delta * FLOW_SERVICE_US * FLOW_AGE_RATE`, so every gap among them is unchanged and
       the triple is blind to precisely the quantity in question. A claim about a measurement that the
       measurement cannot make is the stale-DFAIL shape wearing a number, so it is replaced by the row that
       makes it. */
    int64_t svc_min;
    /* THE OPTIMISM TERM'S OWN COORDINATE, WHICH NO ROW ABOVE CAN STAND IN FOR — completed units of work, the
       quantity `visits` holds. Every other row here is thread time at one scope or another, and while the bonus
       was ALSO thread time the census could not distinguish "this frontier is being served fairly" from "no
       member of this frontier has ever finished anything", which are the two states that produce a busy engine
       with an empty API surface. `vis_max == 0` on a frontier of thousands is that second state, exactly, and
       it is the reading that names it: not one member has reached the end of a program, so not one queued job
       can have run (engine.c's job arms are all under `frame == NULL`). The pair with `vis_min` is the SPREAD,
       which is what says whether the order is concentrating on one member or handing turns round. */
    int64_t vis_min;
    int64_t vis_max;
    /* THE SERVICE OF THE WHOLE FORK FAMILY, IN THE SAME NOTCHES — and it DECIDES THE ORDER now, which is the
       correction this row carries. It was added as a diagnostic beside the ranking, to answer whether the
       reward SCALE was what a run was stuck on: the reward was then copied at every fork while the aging meant
       to cancel it was charged to whichever arm held the thread, so a family with N live arms presented its
       reward N times and paid for it N times over. The reading came back 8910 against an `svc_max` of 1124 — a
       factor of 7.93 — and flow_weight's aging term now reads this quantity instead (flow.c's FlowAcct
       `fam_us`). THE OTHER HALF OF THAT PRESENTATION IS GONE TOO: the reward is held on the same node
       (FlowAcct's `val`), so a family presents it ONCE however many arms it wears, and the two terms are
       finally one account read two ways rather than a credit inflated by N against a debit that is not.
       SO THE PAIR IS READ THE OTHER WAY ROUND FROM HERE ON. `svc_fam_max` is the AGING's own denominator and
       `svc_max` is one member's share of it; their ratio is the fork factor of the widest family in the
       frontier, which is a fact about the DOCUMENT's branching and no longer a defect in the ordering. What
       would be a defect is `svc_fam_max` sitting far below `svc_max`, which cannot happen while every arm's
       charge lands on the family — and flow_age_running asserts the invariant that makes it impossible. */
    int64_t svc_fam_max;
    /* …AND ITS FLOOR, WHICH IS THE HALF `svc_min` COULD NOT SUPPLY AND WHICH CARRIES 93% OF THE TERM. The row
       above says `svc_min` is "the only number in this struct that can answer 'is the aging term measuring
       this flow or the whole frontier'", and that was true of the term the aging USED to read; the term now
       reads `(cpu + fam_us)` and `svc_min` is the floor of the FIRST summand only. So the answer it gives is
       the answer for a fraction of the quantity: measured on the smoke fixture's steady state, `svc_fam_max`
       66580 against `svc_max` 4790 — the family half is 93.3% of the aging, and nothing here reported either
       end of it. A pair of maxima cannot say whether a term ORDERS anything, only how large it is, and those
       are the two readings that took opposite actions on that run: an aging term of 856 points on a frontier
       whose entire weight spread was 0.020.
       WHAT A UNIFORM FAMILY MEANS IS NOT A DEFECT AND MUST NOT BE READ AS ONE. A frontier that is ONE family —
       which a real page's is, every flow descending from the boot flow — reads the identical `fam_us` at every
       member by construction (flow_fork_inherit joins the parent's account), so `svc_fam_max == svc_fam_min` is
       that term contributing a COMMON OFFSET and ordering nothing, which is exactly what flow_silence_notch's
       own comment says the own half exists to cover. The reading is worth having precisely because it is the
       one state the maxima cannot be distinguished from: a genuine multi-family frontier in which one chain is
       monopolising presents the SAME `svc_fam_max` and a floor far below it. */
    int64_t svc_fam_min;
    /* HOW MANY FAMILIES THERE ARE, WHICH IS THE HALF THE PAIR ABOVE CANNOT SUPPLY AND WAS BEING READ AS IF IT
       COULD. The paragraph above says `svc_fam_max == svc_fam_min` is "that term contributing a COMMON OFFSET
       and ordering nothing" — and equality is produced by TWO states that take opposite actions, with nothing
       in a pair of extrema to separate them. On a frontier that is ONE family the equality is an IDENTITY OF
       THE STRUCTURE: every member reads one node's `fam_us` through one pointer (flow_fork_inherit joins the
       parent's account rather than founding a family), so it holds whatever the run does, and the family half
       can never order that document's frontier at all. On a frontier of SEVERAL families the same equality is
       a contingent observation about one instant, which the next charge moves — a term that IS ordering and
       happens to be level. "This term is structurally an offset" and "this term is momentarily level" are
       different findings and the first of them is the one that says stop looking, so reading one number for
       both is how a real ordering defect gets closed as a known constant.
       IT READS EQUAL AT A GENUINE SPLIT TOO, which is why the ambiguity is the ordinary case rather than a
       corner — though no longer for the reason this paragraph used to give. It said a from-baseline flow
       "ARRIVES at the running family's service", and the arrival copies no service at all any more: a new
       family is born at ZERO on both halves, so it reads equal to every other family that has not been charged
       since its own last emission, which on a frontier whose leader keeps emitting is most of them. They
       diverge only once one is charged — flow_age_running bills the running flow's family alone,
       flow_credit_emit zeroes its own alone. A census taken between the split and the first charge is a
       multi-family frontier reading as a single-family one, exactly.
       COUNTED BY IDENTITY, NEVER BY VALUE: two families standing at one service are two families, so the count
       is of distinct family ROOTS reached through the members (flow.c marks each root as the one scan passes
       it). Read beside the pair, the three numbers say which of the two states a run is in: `families: 1` is
       the identity and the term is structurally an offset; `families > 1` with the extrema equal is the term
       level for now; `families > 1` with a floor far below the max is the term ordering, which is the state
       the family charge was added for. */
    long families;

    /* THE @S CANDIDATE SESSIONS, ASKED DIRECTLY, because the first reading of this census had to INFER them
     * and the inference was three fields long: `val_zero` counts members that inherited nothing, `unrun`
     * counts members never charged, `self_emit == 0` is what rules out a just-emitted flow among them, and
     * only all three together said "the members that have never had the thread are the candidates". A fact
     * reached by composing three rows is a fact the next reader composes differently. It is one row now.
     *
     * `cand_unrun` IS THE STARVATION QUESTION AND NOTHING ELSE ANSWERS IT. @PROGRESS's `running` names the
     * INCUMBENT at the sample instant, which for a population that is 2% of the frontier is a 2% coin — it
     * reads as total starvation and as exact fair share with the same numbers, and it was read as the first.
     * A candidate that has held the thread has been charged for it (engine.c charges after every step), so
     * this is how many have never had it, exactly, at the instant it is asked.
     *
     * `cand_dec_max` IS HOW FAR THE BEST OF THEM HAS GOT — the length of the decision vector it stands on,
     * which is how many gates it has replayed and therefore how deep into the program it is. A candidate
     * re-runs from the BASELINE, so arriving at a sink is a distance problem before it is anything else, and
     * §@S requires the search to be DISTANCE-DIRECTED: "a fitness of {filter-survived, sink-reached,
     * context-escaped, handler-fires} the WFQ reads". flow_weight reads the PRE-FIRE rungs of that list
     * (flow_distance), and this row exists because the runway BELOW them was once unmeasured entirely: every
     * rung §@S names reports at a SINK, so a candidate 800 gates into its replay ranked exactly like one 3
     * gates in for the whole of the runway itself.
     * THE LADDER NOW STARTS ONE SITE EARLIER — at the DELIVERY, the source read that puts this flow's bytes in
     * the program — so the first thing a candidate does on the runway is measured and this row is no longer
     * the only witness to the approach. It is still the row that says how much runway there IS, which the
     * ladder cannot: the rung says the bytes are in, this says how many gates the replay crossed to get there,
     * and a candidate stuck at rung 0 with a growing `cand_dec_max` is one whose replay is diverging before
     * its own source read rather than one nobody is serving. It separates three states that look identical
     * from outside: served and
     * progressing (distance — build the fitness), never served (starvation — the ordering), and served while
     * pinned at zero (a candidate being RESTARTED rather than resumed, which no amount of thread time fixes). */
    long cand_members;    /* members carrying a payload substitution — a candidate session OR A FORK OF ONE,
                             because engine.c copies the substitution to the sibling: a candidate that branches
                             is two candidates. So this is the SEARCH's whole live population, and @PROGRESS's
                             `candidates` (the searches SEEDED) is its root count. The pair is a shape test the
                             two numbers make together and neither makes alone: cands == roots * 2^cand_dec_max
                             is a COMPLETE UNIFORM binary tree, which is every candidate stopping at the same
                             depth, and any other value is a ragged one, which is candidates at different
                             depths. Measured once at 144 against 9 roots and a depth of 4 — 9 * 2^4 exactly. */
    long cand_unrun;      /* …of those, how many have NEVER been charged for the thread */
    int64_t cand_svc_max; /* …and the most service any one of them has consumed */
    /* THE DEEPEST DECISION VECTOR AMONG THEM — AND IT COUNTS GATES, NOT STATEMENTS, which is a correction to
       what this row said when it was added. It claimed to be "how many gates it has replayed and therefore how
       deep into the program it is", and the second half does not follow from the first: a flow records a slot
       only at a branch it had not already decided, so a candidate can execute eight hundred statements and
       stand on four decisions, or four statements and stand on four. Read alone the number is uninterpretable
       in the exact way that matters — `cand_dec_max: 4` against a runway of 866 statements reads as "the
       floor" and as "four of the five gates on the path" with the same digit, and those take opposite actions.
       `dec_max` below is the denominator that makes it a reading: an exploring flow that runs the document to
       its end stands on the whole gate sequence, so the RATIO is how far through the gates the best candidate
       has got. Neither number is worth printing without the other. */
    long cand_dec_max;
    long dec_max;         /* the deepest decision vector of ANY member — the gate sequence's own length, which
                             is what the row above is a fraction of */

    /* THE ORDER ITSELF, IN THE UNITS THE PICK USES. `w_top` is what holds the front of the queue and
     * `cand_w_max` is the best any candidate can offer against it, so the GAP between them is the ordering
     * question with no arithmetic in front of it.
     *
     * WATCH `cand_w_max` ACROSS SERVICE, because that is where the term this engine calls aging stops behaving
     * like one. A candidate records no endpoints by design (endpoint_suppress), so nothing it does raises its
     * reward above the one it ARRIVED at until it fires, and flow_weight is then `R + 1/(1+v) - (s+F)*Q*RATE`
     * with `R` fixed — the whole of the movement is in the two charged terms. `R` was written here as 0 on the
     * strength of `val_zero`'s paragraph above, and it is the incumbent's reward now; the arithmetic below is
     * unchanged by that because it is a DIFFERENCE across service and `R` cancels out of it. The arithmetic
     * that stood here was
     * written when the optimism term read `s` as well, and it is corrected rather than kept: it said a
     * candidate at reward 0 was worth 1.000 unserved, 0.488 after ONE quantum and -0.029 after ten, so being
     * handed the thread ten times cost it MORE THAN THE ENTIRE OPTIMISM RANGE. Both halves of that penalty were
     * the same thread time counted twice. With the bonus keyed on COMPLETED UNITS the first quantum costs a
     * candidate 0.012 and not 0.512, and what it pays for a turn is its aging alone — which is the price
     * FLOW_AGE_RATE states and the only one this weight is supposed to charge. The observation the paragraph
     * was making survives: for a flow that cannot emit until it ARRIVES, every charge is pure penalty, so a
     * candidate that has been served ranks below one that has not, and it does so at the rate the rate names.
     * THE ROOT IS THAT AGING IS ABSOLUTE AND §scheduler'S SENTENCE IS COMPARATIVE. "A monopolizer that burns
     * CPU without emitting sinks below productive+unrun flows" is a statement about this flow AGAINST the
     * others; `(s+F) * FLOW_SERVICE_US * FLOW_AGE_RATE` is a statement about this flow alone, so the depth at
     * which a member sinks does not move with what the rest of the frontier consumed. Ten notches on a frontier
     * whose busiest member has burned 489 is scored on the same scale as 489 on a quiet one. The primitive that
     * fixes it is start-time fair queueing's VIRTUAL TIME, "a continuation of an active flow enters at that
     * flow's virtual time, never at the system's".
     * HALF OF IT IS BUILT AND THE HALF IS THE ENTRY, NOT THE TERM. flow.c's flow_arrive_at_virtual_time applies
     * SFQ's `max{v(t), F_prev}` to the three from-baseline entries that were placing a newcomer at virtual time
     * ZERO — ahead of the whole backlog — so a flow can no longer promote the documents and candidate sessions
     * it CREATES above every member already waiting, and it applies it as a CONTINUING relation: an account
     * that has never been served reads the clock rather than a number written when it was created, so it is
     * not left behind by what the incumbent earns afterwards either. What is still absolute is the aging term
     * itself, and the
     * consequence is at LEVEL-1 rather than here: `engine_top_weight` is this function's value with no frame of
     * reference, so a document's best flow falls by one point per second of unproductive CPU without bound while
     * a document that boots today enters at 1.0. Nothing but an EMISSION ever raises a weight, and a document
     * that cannot win the Level-1 pick cannot emit, so the crossing at `(reward + 1)` seconds is a RATCHET: past
     * it a mature document is outranked by every page that arrives afterwards, permanently, for as long as the
     * pool keeps being fed. That is what `svc_min` and `svc_fam_min` above are for — the frame of reference,
     * measured, at BOTH the scopes the aging term is summed over.
     * AND "MEASURED" IS A CLAIM ABOUT AN EMITTED ROW, NOT ABOUT A COMPUTED ONE. This sentence named `svc_min`
     * alone and was written while the only consumer of this struct — engine.c's @WFQ printf — did not print
     * it, so a row that was computed on every census, asserted about in three paragraphs here, and reachable
     * by nobody read as the frame of reference the file kept pointing at. That is the mirror of the
     * defaulted-field defect (§Architecture: a name READ somewhere and WRITTEN nowhere) with the arrow
     * reversed, and it is harder to see because the value is real. Every field of this struct is emitted; the
     * accounting assertion at the end of flow_wfq_census is what keeps that true as terms are added. */
    /* THE FITNESS TERM'S OWN RANGE — §@S's distance, which flow_weight adds and which no row here reported.
       Its FLOOR is not a field because it is a constant of the type: a flow with no payload has no ladder to
       stand on and both fitness writers refuse to record one, so every non-candidate stands at
       exactly 0 and [0, dist_max] is the range rather than an estimate of it. Read beside `cands`: a run with
       `cands: 0` has this at 0 by construction and the fitness term is then ordering nothing — which is a fact
       about the run's @S population and not about the term, and the pair is what separates them. */
    double dist_max;
    double w_top;         /* flow_best's weight — what holds the front of the queue */
    double w_min;         /* the lowest weight in the frontier — the other end of the same order */
    double cand_w_max;    /* the best weight any @S candidate can offer against `w_top` */

    /* THE TWO HALVES OF THE LEADER'S OWN SILENCE, READ OFF THE ONE MEMBER flow_best RETURNED — the pair without
       which `val_top` is a reward nobody can say is being EARNED or merely REMEMBERED. `val_top` is the front
       flow's FAMILY's ledger (flow_reward), so a reward that stops climbing is the leading account having gone
       quiet; but a ledger is monotone, so a FROZEN one and a SLOWLY-EARNING one are the same digit at any single
       census and only differ across a stream. The silence is the half that is not monotone — flow_credit_emit
       zeroes an account's `fam_us` on any arm's emission — so these two rows are what turn "the leading family's
       reward did not move" from an observation into a statement about whether its aging is being FORGIVEN.
       THEY ARE THE TWO HALVES SEPARATELY AND NEVER THEIR SUM, because on the ordinary frontier only one of them
       can order anything and it is not the one a reader reaches for. Every arm of one family reads the identical
       `fam_us` through one pointer (flow_fork_inherit joins the parent's account) and a real page's whole
       frontier is one family, so the family half is a COMMON OFFSET there: it is charged to the leader and to
       every member standing behind it in the same instant, and it therefore cancels out of `never_picked_gap`
       and out of every other difference between two members of one account. `top_svc_fam` climbing on a
       `families: 1` frontier says the leading account is silent and says NOTHING about anyone catching up.
       `top_svc` IS THE ONE THAT MOVES A GAP, and reading it across a stream is what separates two states that a
       count of starved members reads identically. flow_age_running charges the RUNNING flow's own `cpu`, and a
       member that is never dispatched is never charged, so a starved member's own silence is frozen while the
       leader's is not: a genuinely monopolising front sinks, and `top_svc` climbs monotonically as it does. A
       front that is instead being REFILLED — new arms minted at low inherited silence, each taking a turn at the
       head and being replaced by a fresher one — shows `top_svc` staying low or sawtoothing while the same gap
       stands, and no amount of waiting closes it because the flow being charged is not the flow at the front.
       Those two take opposite work (re-price the aging, versus stop the mint from outranking the tail) and
       nothing else in this struct tells them apart: `svc_max` is the largest in the frontier and the leader need
       not be it, and `svc_min` is the smallest and the leader need not be that either.
       IN NOTCHES, exactly as `svc_max`/`svc_fam_max` are, so the three are one unit and a reader multiplying by
       FLOW_AGE_QUANTUM gets points in every row. They are not `flow_silence_notch`, which is a floor of the SUM
       and is therefore not the sum of these two — that function is what the WEIGHT charges, and these are what a
       reader ATTRIBUTES it to; the discrepancy is at most one notch and it is why the halves are reported rather
       than the total the weight uses. Bounded by the extrema above by construction (the leader is one of the
       members this same scan walks) and asserted there, so a value outside them is the census and the pick
       having stopped walking one population. */
    int64_t top_svc;
    int64_t top_svc_fam;
    /* HOW MANY TIMES THE LEADING ACCOUNT'S WITHIN-FAMILY ORDER HAS BEEN ERASED — its `emit_gen`, which is
       raised once per credited finding by flow_credit_emit and written by nothing else, so it IS that count
       exactly rather than an estimate of it. `val_top` cannot stand in for it: a credit is any positive amount
       (flow_credit_emit asserts only `v > 0.0`, and the @S survival ratchet credits the increment between two
       fractions), so a ledger in points is not a count of events and this file already forbids deriving an
       exactness argument from "`val` is integral".
       WHAT IT IS FOR, AND IT IS THE ONE ROW THAT CAN DECIDE THE SWEEP QUESTION `picks_max` RAISES. An emission
       forgives the whole account's silence window in one statement — `fam_us` to zero and the generation bumped,
       which makes flow_own_silence answer ZERO for every arm of that family at once. Both halves of the aging
       therefore read zero for every member simultaneously, and the weight collapses to the reward (common,
       through one pointer), the fitness (zero for a non-candidate) and 1/(1+visits). Every arm inside one visit
       tier is then EXACTLY tied, and flow_pick walks the registry — birth order — returning the first maximum,
       so the tier is swept from its OLDEST member forward, one member per quantum of own silence, and the next
       emission restarts that walk at the head of the registry. The own-silence charge is the only thing that
       advances the sweep, and this is the count of the events that erase it.
       SO THE READING IS AN ARITHMETIC ONE, which is what makes it falsifiable rather than a story: if the sweep
       restarts, `picks_max` tracks THIS number within a small factor (the oldest member of the top tier is
       re-picked about once per restart) and `picks_live / picks_max` is the mean sweep DEPTH between two
       emissions. `picks_max` far below this says members are sinking for good and something else drains the
       cohort; this at or near zero while `picks_max` is not says the erasure is not happening at all and the
       restart reading is dead. `val_top / top_forgiven` is a second reading nobody had: whether the leading
       account is earning findings or ratcheting fractions.
       ITS KIND IS A COUNTER AND ITS SERIES IS NOT, WHICH IS THE TRAP THIS ROW WOULD OTHERWISE WALK INTO. The
       quantity is a lifetime count OF ONE ACCOUNT, and which account is at the front can change between two
       censuses — so the SERIES may fall, and a fall is a LEADER CHANGE rather than a counter running backwards.
       `val_top` moves with it and is what tells the two apart: both falling is a new account at the front; this
       falling while `val_top` rises is one account's generation having gone backwards, which nothing may do.
       Read it per sample against `picks_max`, never differenced on its own. */
    int64_t top_forgiven;
    /* THE MOST EVERY TERM OF THE ORDER EXCEPT THE REWARD CAN LIFT ONE MEMBER — flow.c's FLOW_NONREWARD_MAX,
       carried out of the engine rather than restated by whoever reads the gaps. It is the bound `flow_nonreward`
       asserts on every weight it computes, so it is the number that decides whether a member standing behind the
       front is behind by LIFT (a bounded term reading differently could put it there) or behind by AGING (its
       non-reward sum is negative, and nothing bounded reaches it — only the leader sinking).
       IT IS EMITTED RATHER THAN GREPPED BECAUSE IT IS A DERIVATION AND NOT A NUMBER. build.mjs reads plain
       `#define`s straight out of the host (`hostDefine`), and this one folds three terms together — the optimism
       ceiling, the fitness ladder over FLOW_RUNGS_N, and the aging's zero — so a reader that re-derived it would
       be the second copy of a rule §Architecture's auditor sentence forbids, and it would go stale the day a
       rung is added beneath the ladder without anything saying so. Emitted, the day the ladder changes the
       bound changes with it and no reader has to know.
       WHAT A READER DOES WITH IT is stated once, here, so the two ends cannot drift: a member's weight is its
       ACCOUNT's reward plus its non-reward sum, so a difference between two members' weights is at most their
       reward difference plus this — and `val_top - val_min` bounds that reward difference over the members this
       scan walks. So `(val_top - val_min) + nonreward_max` is the largest gap that a non-negative non-reward
       sum can produce, and a gap ABOVE it is the arithmetic saying the trailing member's own terms are already
       net negative. On a one-family frontier the reward difference is identically zero (every member reads one
       account), and the bound is this row alone. */
    double nonreward_max;

    /* THE JOB BACKLOG, SPLIT BY WHAT EACH JOB IS WAITING ON — three states that the cold census's `jobs` total
     * reports as one number, and the three take OPPOSITE actions.
     *
     * §scheduler says "every enqueued job is a first-class flow in the one WFQ" and "there is NO
     * `while(JS_ExecutePendingJob)` loop — the scheduler IS the job pump", so a queued job that never runs is
     * either a ROUTE that cannot reach it (§scheduler's razor: a resume that "drops, starves, skips, reorders,
     * or forgets ANY flow" is a cap) or an ORDER that has not got to it yet (the WFQ working as specified, its
     * terms mis-scaled). Those have opposite fixes and `jobs: 5814` on the cold line is the same digit for
     * both — which is how a whole class of continuation came to be read as unreachable when it was outranked.
     * The split is over exactly the two predicates flow_step and flow_pick already ask, so it is a reading of
     * the engine's own decisions and not a fourth opinion beside them:
     *
     *   `jobs_owed`   — the member carries the host-owed mark, so flow_pick REFUSES it (`runnable_only`).
     *                   These jobs wait on the HOST, and nothing about the order can move them.
     *   `jobs_framed` — the member is inside a program (`frame != NULL`). HTML §8.1.4.4 "Calling scripts",
     *                   clean up after running script step 3 — "If the JavaScript execution context stack is
     *                   now empty, perform a microtask checkpoint" — is what every job arm of flow_step is
     *                   under, and `Flow::frame` IS that stack here. So these jobs wait on the member
     *                   COMPLETING its unit of work, which is also what advances the optimism term's `visits`.
     *                   This row is NOT a defect on its own: it is the spec's precondition, measured.
     *   `jobs_ready`  — neither: an empty stack and no mark, so the member reaches its jobs at the very next
     *                   pick it wins. These jobs wait on RANK ALONE, and they are the population §scheduler's
     *                   WFQ sentence is about.
     *
     * Disjoint and exhaustive by construction (two booleans over every member), which is the point: a fourth
     * reason cannot be folded silently into one of the three, because there is nowhere for it to go.
     *
     * `job_w_gap` IS THE READING THE THREE COUNTS CANNOT MAKE, and it is denominated in the order's own unit.
     * It is `w_top` minus the best weight any READY holder offers, so it says how many reward points the job
     * backlog is standing behind the front of the queue — 0 means the top of the order itself holds a runnable
     * job and the backlog is not an ordering problem at all; a figure on the scale of the reward spread
     * (`val_max - val_min`) is the ordering saying that the aging term, which moves at FLOW_AGE_QUANTUM per
     * quantum of silence, cannot reach it inside a session. READ IT BESIDE `jobs_ready` AND NEVER ALONE: with
     * no ready holder there is no gap to state, and this is 0 for that too. The pair is what separates
     * "nothing is waiting on rank" from "the front of the queue is a job holder", which are the two states a
     * bare 0 reads as. `>= 0` by construction — `w_top` is flow_best's maximum over the same members this scan
     * walks — so a negative value is the pick and the census disagreeing about the one comparator, which is
     * the edit the DCHECK beside it catches.
     *
     * `vis_zero` IS THE OTHER HALF OF `jobs_framed`, counted over MEMBERS rather than over jobs: how many of
     * them have completed no unit of work at all. `vis_min: 0` says at least one and a frontier of thousands
     * makes that unremarkable; the COUNT is what says whether the framed backlog belongs to a handful of deep
     * programs or to most of the frontier. It is also the population whose optimism bonus can never decay
     * (flow_queue_weight keys it on completed units), so a large `vis_zero` beside a large `jobs_framed` is a
     * frontier ranking itself on a term none of its members can spend. */
    long jobs_ready;
    long jobs_framed;
    long jobs_owed;
    double job_w_gap;
    long vis_zero;

    /* THE DELIVERY BACKLOG, SPLIT THE SAME WAY AND FOR THE SAME REASON — the missing twin of the four rows
     * above. The cold census says how many register entries are ANSWERED AND UNTAKEN (`pendReady`) and how
     * many members could take one right now (`canDeliver`); neither says WHERE THOSE MEMBERS STAND IN THE
     * ORDER, and that is the one question a debt of hundreds of thousands of answered replies against a
     * handful of deliveries reduces to. `jobs_ready`/`job_w_gap` already ask it of the job backlog. Nothing
     * asked it of the reply backlog, which is the larger of the two by orders of magnitude.
     *
     * THE THREE ARE OVER MEMBERS, WHERE THE JOB ROWS ABOVE ARE OVER JOBS, and the difference is deliberate
     * rather than an inconsistency. `flow_job_pending` is a field read; `pending_deliverable_count` is a WALK
     * of a register that holds hundreds of entries, and this scan already runs over every member of a frontier
     * in the thousands — cold_census pays that walk once per report and a second copy of it here would double
     * it to say something the first already says. `pending_ready` short-circuits at the first deliverable
     * entry, so what this asks is the cheap half: not how big the debt is, but WHO is holding it.
     *
     * Disjoint and exhaustive over the members that hold one, in the order the engine asks them:
     *
     *   `deliv_owed`   — the member carries the host-owed mark, so flow_pick REFUSES it (`runnable_only`) and
     *                    no ranking can move it. On a frontier whose registers hold nothing OUTSTANDING this
     *                    should be zero, because flow_set_host_owed's own DCHECK admits a mark only for an
     *                    outstanding entry or a referenced document — so a non-zero row here beside
     *                    `pendReady == pend` is those two statements disagreeing.
     *   `deliv_framed` — the member fails flow_stack_empty, so the reply-delivery arm cannot run for it. This
     *                    is HTML §8.1.4.4 "Calling scripts"'s clean up after running script step 3 measured,
     *                    not a defect on its own — exactly as `jobs_framed` is not.
     *   `deliv_ready`  — neither: the arm's whole guard holds and the pick will consider it, so this member's
     *                    reply waits on RANK ALONE. It is the population §scheduler's WFQ sentence is about.
     *
     * `deliv_ready` IS NOT THE COLD CENSUS'S `canDeliver` AND THE DIFFERENCE IS ITSELF A READING. That row is
     * `flow_stack_empty && pending_ready` and this one subtracts the host-owed marked members, so
     * `canDeliver - delivReady` is exactly the population the ARM could serve and the PICK will not offer the
     * thread to. Two questions, two answers, and neither is a second spelling of the other — which is why they
     * are not unified.
     *
     * `deliv_w_gap` IS THE READING THE COUNTS CANNOT MAKE, denominated in the order's own unit, exactly as
     * `job_w_gap` is: `w_top` minus the best weight any READY holder offers. 0 means the front of the queue
     * ITSELF is holding an undelivered reply and the backlog is not an ordering problem at all. A positive
     * figure is readable against the terms that produce it, which is the whole value of stating it in this
     * unit rather than in members: one completed unit of work costs a member its optimism bonus from
     * 1/(1+v) to 1/(2+v) — HALF A POINT at v=0 — while the aging term moves at FLOW_AGE_QUANTUM per quantum
     * of silence, which is ENGINE_QUANTUM_MS/1000 of a point. So a gap near 0.5 says the ready holders are one
     * completed unit behind the front, and a gap of many multiples of FLOW_AGE_QUANTUM with `vis_zero` large
     * says they are behind a population whose optimism bonus none of its members can spend. READ IT BESIDE
     * `deliv_ready` AND NEVER ALONE: with no ready holder there is no gap to state and this is 0 for that too.
     * `>= 0` by construction, for `job_w_gap`'s reason exactly, and asserted beside it. */
    long deliv_ready;
    long deliv_framed;
    long deliv_owed;
    double deliv_w_gap;
} WfqCensus;
void flow_wfq_census(WfqCensus *out);

/* WHAT THE ORDER COSTS TO ASK, WHICH IS A DIFFERENT QUESTION FROM WHAT IT DECIDES AND HAS NO ROW ANYWHERE.
 *
 * THE FOUR ENTRIES ABOVE ARE ONE SCAN, AND THE SCAN IS LINEAR IN THE FRONTIER. That is not a defect on its own
 * — flow_weight is O(1) by construction (the preempt hook's own note says it "may not walk", because the hook
 * reads it per opcode) — but it makes the ASK's cost a function of the frontier's SIZE, on an engine whose
 * frontier grows because forking is the point. Nothing measured it, so "the tail is not being reached" had one
 * reading available and two causes: not enough thread time exists for the members standing, or the thread is
 * being spent asking the order rather than running it. Those take opposite work and no row separated them.
 *
 * WHY THE ENTRIES ARE COUNTED APART AND NOT SUMMED. They run at DIFFERENT CADENCES, which is the whole reading:
 *   `next-to-run` is the dispatch loop's, ONE per step by construction, so its weight total over `steps` is the
 *      average frontier a step pays for.
 *   `rival-of-incumbent` is the PREEMPT HOOK's, and its cadence is the frontier's GENERATION — every fork,
 *      arrival, departure and emission calls frontier_rank_changed, so the hook's cached rival goes stale and
 *      the next opcode rescans. A forking page therefore pays this one per fork rather than per step, which is
 *      a rate nothing about the dispatch loop would predict.
 *   `best` and `eviction-tail` are the host's and the pager's, asked per report and at the RAM floor.
 *   `wfq-census-walk` is the REPORT's own, one per sample — the instrument measuring what the instrument costs.
 *      There are TWO samplers and a smoke's count is both of them: the result document's composer, which is the
 *      only one the shipped program has, and the native fixture's probe table. That matters the moment somebody
 *      compares a fixture number against a shipped one.
 * Summed, a scan the hook made per fork and a scan the loop made per step are one number, and the two take
 * opposite work — the same collapse `resume-program` carried until solver/step_unit.h split it.
 *
 * AND THE LAST ENTRY IS THERE BECAUSE AN INSTRUMENT WHOSE OWN COST IS UNMEASURED IS THE DEFECT THIS FILE
 * ALREADY NAMES ONE LEVEL UP. §Testing's rule is that a gate reading a tree no revision contains measures
 * nothing; an instrument heavy enough to change the run it samples is the same fault wearing a census, and it
 * cannot be argued about — a count is the only thing that settles it, because this host's quantum is
 * wall-denominated and a duration would be about the machine. The census is the natural place for it to hide:
 * it is O(members) in a frontier that grows because forking is the point, it calls flow_weight AND
 * flow_distance AND decide_blob_stats per member, and at APICLIENT_DEV=1 — which every smoke is — the asserts
 * inside those are live. Read `scanCensusWeights / scanCensusRuns` for the mean frontier a sample paid for and
 * `scanCensusWeights` against `scanNextWeights` for what fraction of all frontier-weighing the REPORT is,
 * rather than the run. A census is worth its cost; a census nobody can price is not a measurement of anything.
 *
 * IT IS A COUNT AND NOT A CLOCK, deliberately and for §Testing's reason: a measurement a loaded machine can
 * falsify is not a measurement, and this host's quantum is wall-denominated. Scans and weight evaluations are
 * things the engine DID — being descheduled cannot inflate either — so these numbers are comparable between two
 * runs on a machine under any load, which is exactly what a duration here would not be.
 * WEIGHT EVALUATIONS AND NOT LOOP TRIPS: the scan skips the excluded member and the host-owed ones without
 * pricing them, so trips would overstate what a filtered scan costs. What is counted is the flow_weight the
 * scan itself performed — its seed's and its loop's — and never the ones a DCHECK below it makes, which do not
 * exist in the build the product ships.
 * IT DECIDES NOTHING. No weight term reads it, no pick branches on it, nothing is bounded by it; it is a report,
 * and a scheduler that consulted its own cost would be ordering on a quantity that is not about any member. */
#define FLOW_SCANS(X)                                                                     \
    /* the dispatch loop's pick — one per step */                                         \
    X(NEXT,  "next-to-run")                                                               \
    /* the preempt hook's rival rescan — one per frontier-generation change */            \
    X(RIVAL, "rival-of-incumbent")                                                        \
    /* the host's best-weight read and the pager's tail, per report and at the RAM floor */\
    X(OTHER, "best-and-eviction-tail")                                                     \
    /* the CENSUS's own walk — one per sample, and the only frontier-weighing walk in this  \
       engine that nothing priced. flow_wfq_census weighs every member itself AND calls     \
       flow_best, which weighs every member again under OTHER, so a sample costs TWO        \
       weighings of the frontier and only one of them was visible. Counted apart from       \
       OTHER for the same reason the three above are counted apart: its cadence is the      \
       REPORT's, so summing it into a scan the dispatch loop makes per step would put the   \
       instrument's own cost inside the rate that exists to price the dispatch. */          \
    X(CENSUS, "wfq-census-walk")
#define FLOW_SCAN_ENUM(id, name) FLOW_SCAN_##id,
typedef enum { FLOW_SCANS(FLOW_SCAN_ENUM) FLOW_SCAN_N } FlowScan;
#undef FLOW_SCAN_ENUM
#define FLOW_SCAN_CASE(id, name) case FLOW_SCAN_##id: return name;
static inline const char *flow_scan_name(FlowScan s)
{
    switch (s) { FLOW_SCANS(FLOW_SCAN_CASE) case FLOW_SCAN_N: break; }
    DFAIL("an order scan reported an entry that is not in solver/flow.h's list — the enum and the name are two "
          "expansions of ONE macro, so a value outside it did not come from an assignment at a flow_pick call "
          "site; it is a cast or an uninitialised read");
    return "(not a scan entry)";
}
#undef FLOW_SCAN_CASE

/* HOW MANY SCANS EACH ENTRY MADE, AND HOW MANY MEMBER WEIGHTS THEY EVALUATED — lifetime, per instance. Read as
   a PAIR: the count alone says how often the order was asked and the weights say what asking it cost, and the
   quotient is the frontier the scan actually walked, which no other row carries. Both are `long` and neither
   is reset.
   THE WEIGHT COUNT CAN BE ZERO ON A NON-EMPTY FRONTIER, and a reader that treats that as a broken counter will
   fire on a real state: the runnable-only scans skip a host-owed member BEFORE pricing it, so a frontier every
   member of which is waiting on the host prices nobody — which is the STALL engine.c names at the pick's own
   `if (!best) break`. So there is no floor to assert between these two rows and none is asserted. */
long flow_scan_runs(FlowScan s);
long flow_scan_weights(FlowScan s);

/* HOW MANY TIMES THE ORDER CHANGED — the denominator the hook's rescan count has and `scanNextRuns` is NOT,
 * and without which the two readings of that count disagree with each other.
 *
 * MEASURED, ON TWO ADJACENT CENSUSES OF ONE RUN: `scanRivalRuns / scanNextRuns` read 3.22 over the run's whole
 * life and 0.86 over its last interval — one says the hook rescans three times per step and the other says it
 * rescans less than once, and NEITHER is wrong. They are answers to a question whose denominator is not the
 * step: the hook rescans when `flow_frontier_gen() != g_seen_gen || cur != g_seen_cur` (engine.c), so its
 * cadence is set by RANK CHANGES and incumbent switches, not by steps. A step during which a flow forks three
 * times moves the generation three times and costs three rescans; a step during which nothing forks costs at
 * most one. So a rate per step is a COST (how much scan work a step pays for) and a rate per rank change is
 * the CACHE's own hit rate, and the two were being read as one number.
 * WHICH IS ALSO WHY THE DISAGREEMENT IS INFORMATIVE RATHER THAN NOISE: in that run the frontier grew by ONE
 * member across the interval where the ratio fell to 0.86, so the forking had all but stopped and the rescans
 * fell with it. That is the mechanism behaving exactly as described and NOT the hypothesis failing — but it is
 * indistinguishable from the hypothesis failing while the only denominator available is the step count, which
 * is precisely what this row is for. It settles nothing on its own; it makes the question answerable.
 *
 * IT IS THE RAISE COUNT AND NOT `flow_frontier_gen`, and the difference is a lifetime rather than a spelling:
 * flow_registry_init resets the generation and resets none of the scan counters, so a ratio built on the
 * generation number would be two quantities over two lifetimes the first time a host initialised a second
 * registry. Counted at the raise, it is commensurable with the scan rows by construction.
 * IT DECIDES NOTHING, for the scan counters' reason exactly. */
long flow_rank_changes(void);

/* HOW MANY DISPATCHES THE TIE-BREAK DECIDED AGAINST A STARVED MEMBER — §scheduler's razor's STARVES, made
   countable at the line that chooses. LIFETIME counter; the only kind a reader may difference, and it is read
   against `picks_lifetime`, because the FRACTION is the reading and the raw count is not: a handful over a
   session is the strict comparison doing its job, and a figure on the order of the dispatches themselves is
   the ORDER having stopped separating members while the pick's registry position decides which one runs.
   IT IS NOT A FOURTH GAUGE BESIDE `never_picked`, `never_picked_gap` AND `never_picked_at_top`, and the
   difference is what it is for. Those three are taken over the frontier at an INSTANT and can say only that a
   tied tail exists; none can say whether a PICK ever passed over one, which is the actual claim the razor
   makes. Measured, and it is why this exists: a frontier that GROWS BY FORKING makes all three uninformative
   at once — `never_picked` climbing is arithmetic about the fork factor rather than about the order (a run
   creating 5786 flows against 1010 dispatches cannot reach them whatever the order says), `picks_max` cannot
   fall toward one while a framed flow legitimately needs many quanta to finish a program, and
   `picks_live / (members - never_picked)` sums re-dispatches that CONTINUE a program, which are necessary,
   with re-dispatches that pass over a starved member, which are the defect. This counts only the second. */
long flow_starved_picks(void);

/* The highest-priority flow in the frontier, or NULL if empty — EVERY member, whether or not it can currently
   make progress. It answers the host's Level-1 question (this document's best weight) and the census's; the
   scheduler's own pick is flow_next_to_run below. Does not remove it. */
Flow *flow_best(void);

/* WHICH FLOW SHOULD HOLD THE THREAD, given the one that holds it now (NULL when nobody does) — the dispatch
 * loop's PICK. The same comparator and the same order as flow_best, with two things said on top of it, and
 * both of them are what separates a RANKING from a SCHEDULE:
 *   - a flow that has reported itself host-owed is not a candidate (it cannot use the thread, so handing it
 *     over hands it straight back), and
 *   - the INCUMBENT keeps the thread unless a candidate is STRICTLY better — the identical comparison the
 *     preempt hook's value clause makes, so the two ends of one decision cannot disagree. A tie is not a
 *     reason to swap two COW deltas.
 * NULL means nothing can run: either the frontier is empty, or every member is waiting on the host — which is
 * the STALL, decided by asking each member rather than by counting a run of unproductive picks.
 * `why` NAMES THE ASKER AND IS NOT DERIVED FROM THE ARGUMENTS, because two callers ask this identical question
 * for different reasons and at different cadences: the DISPATCH LOOP asks it once per iteration to decide who
 * holds the thread, and the HOST asks it per poll for its Level-1 weight (engine_top_weight). Their costs are
 * the same scan and their meanings are opposite — one is what a step pays, the other is what a report pays —
 * so summing them would put a per-poll cost inside the per-step rate that FLOW_SCANS exists to make readable.
 * The site travels with the operation; nothing here can infer it, because the arguments are identical. */
Flow *flow_next_to_run(const Flow *incumbent, FlowScan why);

/* WHO THE RUNNING FLOW IS DEFENDING AGAINST — the best flow that could USE the thread, other than `cur`. The
 * preempt hook compares it against the running flow itself, which is why this one excludes rather than seeds.
 * It is the same scan, so the hook and the pick can never rank two flows differently. */
Flow *flow_rival_of(const Flow *cur);

/* HOW MANY WHOLE QUANTA OF THREAD TIME THIS FLOW HAS CONSUMED — the quantised reading of `cpu`, and a CENSUS
 * quantity: it says who is consuming the thread, at the granularity the thread is handed out in. It is not
 * itself a term of the weight (it used to be the optimism term's, and a microsecond is not a visit), but the
 * aging term reads the SUM of it and the row below in exactly this unit. */
int64_t flow_service_notch(const Flow *f);
/* …AND THE FAMILY'S, in the same unit. Two quantities with two reset points (flow.c's flow_age_running says
   why); their RATIO is the fork factor of the widest family in the frontier. */
int64_t flow_family_notch(const Flow *f);
/* THE AGING TERM'S QUANTITY — how many whole COOPERATIVE QUANTA of silence this flow stands at: its own thread
   time since its last emission plus its fork family's since any arm of it last emitted, in the unit of the two
   notches above and deliberately the SAME one. The PRICE is applied where the term is summed (flow.c's
   FLOW_AGE_QUANTUM), because what a microsecond costs and the smallest step the order can express are two
   quantities and one constant cannot be both: this used to step in whole emitted FINDINGS, 83 quanta wide, so
   a flow that consumed a slice of the thread had its rank unchanged and the pick that immediately followed
   read a weight the charge had not moved. Public because it is half of what a rank CHANGE is made of: between
   two of these notches, and between two of the flow's completed units, its weight cannot move except through
   an emission. That pair is exactly the invariant engine.c's seam assertion holds the value yield to. */
int64_t flow_silence_notch(const Flow *f);

/* THE LOWEST-PRIORITY MEMBER OTHER THAN `exclude` — the TAIL the cold tier gives up first at the RAM floor, and
 * the SAME comparator as flow_best read in the other direction. Not a second ranking: the flow that is paged
 * out has to be the flow the WFQ would have run last, or the engine evicts what it was about to do and keeps
 * what it was starving. Asserted where it is computed, by RE-DERIVING the minimum over the same candidate set
 * — not, as it used to say here, "against flow_best": that comparison was a minimum against a maximum, which
 * is arithmetic and holds for every frontier that can exist.
 * EVERY MEMBER, not only the runnable ones — a flow waiting on the host is the cheapest thing here to page (its
 * recipe re-issues the request and gets today's answer), and filtering it out would leave the flows that cannot
 * run holding the RAM the flows that can need.
 * `exclude` is the flow the scheduler is switched into, which is the one member that can be neither written out
 * nor released: its decision state is live in decide.c and its delta is applied to the live heap. */
Flow *flow_worst(const Flow *exclude);

/* THIS FLOW ANSWERED FLOW_STEP_OWED. It stays in the frontier at its own weight and keeps every work item it
   holds — nothing is dropped, removed or reordered; it is simply not PICKED again until the HOST does something
   that could have answered it. Asserts that the flow was not already marked, which is the two-sided half of
   that sentence: a marked flow is out of the pick, so a second report means its mark was laundered. */
void  flow_set_host_owed(Flow *f);

/* HOW MANY MEMBERS HAVE REPORTED THEMSELVES HOST-OWED — the scheduler stating a fact about itself, beside the
   census's `blocked` (which asks the REGISTER whether the host owes this flow anything). The two answer
   different questions and the gap between them is the diagnostic: `blocked: 512, owed: 59` is a frontier whose
   marks are being cleared faster than the sweep can lay them down, which is exactly the state that made a
   fully-blocked document swap COW deltas 1.76 million times instead of reporting STALLED. Equal numbers on a
   stalled frontier is the healthy reading. */
int   flow_host_owed_count(void);

/* THE HOST ANSWERED THIS FLOW, so it is askable again. ONE CLEAR PER EVENT, ON THE FLOW THE EVENT REACHED —
 * a reply provided into its register, an answer delivered to its request, a record or an operation the host
 * attached to it. Those are the only things that can change a host-owed flow's answer, and each of them names
 * the flow it changes.
 *
 * IT USED TO BE CLEARED AT THE TOP OF EVERY SLICE, on the reasoning that "between two slices the host ran".
 * That is true of a slice that ended because the engine had nothing left to do, and FALSE of one that ended on
 * its CPU QUANTUM — the cooperative yield is about thread-sharing, and the host it hands the thread to has
 * nothing to answer. So the mark was being laundered by the one slice exit that means nothing: measured on a
 * document whose entire frontier was blocked (512 of 512), a slice marked the ~59 flows it had time for, the
 * quantum cut it short, and the next slice re-admitted all of them. The sweep never reached the end of the
 * frontier, the STALL was unreachable BY CONSTRUCTION, and the engine swapped COW deltas 1.76 million times
 * with not one flow finishing. Tying the lifetime to the EVENT rather than to the slice is what makes "every
 * member is waiting on the host" a state the scheduler can actually arrive at. */
void  flow_clear_host_owed(Flow *f);

/* …AND THE ONE EVENT THAT NAMES NO FLOW: an external document script's text is the DOCUMENT's, so the flow
   that delivers that reply fills a slot every other flow parked on the same script index was waiting for. It is
   the only unblocking that happens inside a slice, which is why it is the only clear that is not per flow. */
void  flow_clear_host_owed_all(void);

/* A counter bumped on every frontier membership change (add/remove). The value-yield recomputes its rival
   only when this changes (or the running flow switches), never per-opcode. */
unsigned flow_frontier_gen(void);

/* The running flow (scheduler-set). Detectors credit emitted value to it; the scheduler ages it. */
void  flow_set_running(Flow *f);
Flow *flow_running(void);
void  flow_credit_emit(double v);   /* a NEW @H/@S from the running flow: raise reward, reset aging */
/* WHERE THIS CANDIDATE'S OWN BYTES STAND ON §@S's LADDER — the fitness term of flow_weight, composed from the
   two rung fields rather than stored, so there is no second copy of the order to go stale and no writer that
   can forget to keep one. `(cand_surv + cand_rung) / FLOW_RUNGS_N`, in [0,1], and exactly 0 for every flow
   that is not a candidate — which is not a special case in the arithmetic but the truth about a flow with no
   payload, asserted at both writers rather than assumed here. */
double flow_distance(const Flow *f);
/* THE RUNNING CANDIDATE'S OWN BYTES SURVIVED THIS MUCH OF THE PAGE — §@S's FIRST rung, written where a fitness
   goes. `frac` is the fraction of this flow's payload a re-execution delivered, in [0,1]. It RAISES `cand_surv`
   and does nothing when the flow already stands further along, so the quantity is monotone per flow and an
   observation can never demote the flow that made it. Not a credit: nothing is added, nothing accumulates, and
   this may be called for the same fraction any number of times. It bumps the frontier generation exactly as an
   emission does, because a weight that moves without one is a rank the value-yield cannot see changing.
   IT ASSERTS THE RUNG BELOW IT, which is the flow-side half of the precondition solve.c's three candidate-arm
   sink entries state: a fraction is the surviving run of THIS flow's payload, so a flow whose payload is not
   in the program has nothing for the number to be a fraction of and the run found is the page's own text. */
void  flow_observe_survival(Flow *f, double frac);
/* …AND THE RUNGS THAT THIS FLOW'S BYTES HAVE NOW REACHED — FLOW_RUNG_DELIVERED, _ARRIVED or _ESCAPED.
   SAME RULES, SECOND QUANTITY, AND IT IS A SEPARATE ENTRY POINT BECAUSE THE OBSERVATIONS ARE MADE AT SEPARATE
   SITES AND NO SITE CAN SEE ANOTHER'S ANSWER. The delivery is observed where the substitution is PERFORMED
   (solver/concolic.c), which is a source read and is in a different component from every other site here; the
   survival fraction is measured at every code-execution sink, class-independently; the two sink rungs are
   measured inside the candidate's OWN class, by that class's own language.
   A single "set the distance" entry would force one of them to compose a number out of a quantity it does not
   hold, and the way that goes wrong is silent — it reads back the other rung from the composite and rounds.
   MONOTONE AND ORDERED: it raises `cand_rung` and never lowers it, and it refuses a rung whose predecessor
   this flow has not stood on, because a ladder whose rungs can be taken out of order is a ranking in which
   "escaped" and "arrived and escaped" are the same number. */
void  flow_observe_rung(Flow *f, int rung);
/* THE RUNNING FLOW JUST TOOK AN ARM ITS CONCRETE EXAMPLE CONTRADICTS — `path_forced`'s ONE writer, and the
   whole of what makes a request this flow goes on to build FORCED rather than DERIVED. Idempotent and
   monotone: a path cannot un-take an arm, and the second contradiction says nothing the first did not.
   IT IS NOT A CREDIT AND NOT A CHARGE. Nothing about the rank moves here — a fork is rank-neutral and a
   contradicted arm is still a fork — so this deliberately does NOT bump the frontier generation the way
   flow_credit_emit and the two fitness writers above do: no weight changed, so no rival needs recomputing. */
void  flow_mark_forced_arm(void);
/* HAS THIS FLOW'S PATH STOOD ON ONE — read at the PARK, never at the join, because a park is a work item and
   §scheduler's "an operation that becomes a work item takes its inputs with it" applies to its provenance
   exactly as it applies to its address: a flow that parks a request and takes a contradicted arm afterwards
   built that request on the path it had THEN. */
int   flow_path_forced(const Flow *f);
/* CHARGE THE RUNNING FLOW FOR THE THREAD TIME A STEP JUST BURNED, in MICROSECONDS — the same currency as the
   reward above, which is the only reason the aging term can ever outweigh it. Charged AFTER the step, because
   the quantity is not known before it, and by the scheduler alone (it is the only caller that holds both ends
   of the interval). Never a step count: see the `cpu` field. NOT the only charge on it — a flow that LEAVES the
   frontier hands what it burned to the flow that forked it, which is what makes the term price a fork CHAIN. */
void  flow_age_running(int64_t us);

/* THE FORKED SIBLING TAKES OVER ITS PARENT'S ACCOUNT — both terms of the rank, at the instant of the branch.
   A fork copies every other field of the parent's history (frame, delta, DOM base, jobs, replies, chunks) and
   used to leave these two at the constructor's zeros, which told the WFQ that a flow running since boot had
   emitted nothing and consumed no thread — the second of those being the FULL never-run optimism bonus. The
   sibling then outranked every flow that had ever had a turn, so the frontier could only be entered and never
   drained. Called by the fork and by nothing else; a from-baseline flow (the first flow, a candidate session,
   a cold resume) keeps the zeros, which is what makes ITS bonus mean what it says. Asserts that a fork is
   RANK-NEUTRAL — see the reasoning at the definition.
   IT ALSO RECORDS THE FORK EDGE, and the two halves are one statement. Inheriting says where the arm ENTERS the
   queue; the edge says where the thread time it goes on to burn ENDS UP, and for an arm that runs the rest of the
   document and finishes the answer used to be nowhere. A fork chain is charged as one monopolizer because it
   enters as one flow's continuation. */
void  flow_fork_inherit(Flow *sib, const Flow *parent);

/* RELEASE A FLOW THE SCHEDULER IS NOT SWITCHED INTO — the ONE teardown for a member of the frontier, and the
 * primitive the PARTIAL self-park needs (§scheduler: "an engine self-parks its residue to the IDB cold tier
 * under pressure"; §Time-travel-resume: "under RAM pressure the cold low-value tail serializes to IDB").
 *
 * IT TAKES THE FLOW OUT OF THE FRONTIER AND GIVES ITS RAM BACK — its suspended frame chain, its heap COW delta,
 * its DOM head and its reference on the document's frozen chain, its decision and pin blobs, its chunk bodies,
 * its queued jobs and the replies the host owed it. Everything is released as PARKED state: the delta's head is
 * freed rather than unapplied, and only what the release actually FREES is walked back out of the live heap and
 * document (cow_delta_release / dom_base_release), so releasing a low-value tail while another flow runs cannot
 * disturb the flow holding the thread. Switch the flow out first; both halves assert that you did.
 *
 * WHY IT IS THE ONLY TEARDOWN. The same fourteen fields were released in two other places — the frontier's own
 * teardown and the scheduler's finish path — and a list restated is a list that drifts: the finish path grew a
 * park claim the teardown did not make, and the teardown freed a delta the finish path had already
 * unapplied differently. A field added to `Flow` now has exactly one place that must learn about it, and
 * `flow_remove` asserts from the other side that it did. */
void  flow_release(JSContext *ctx, Flow *f);

/* Remove + free a flow whose state has already been released. Asserts what flow_release owes it. */
void  flow_remove(JSContext *ctx, Flow *f);

int   flow_count(void);
/* IS THIS POINTER STILL A MEMBER OF THE FRONTIER? Pure and side-effect-free, so a DCHECK may ask it. A Flow* is
   held across a return to the host (engine.c's g_sess_cur) and across a switch-out, and nothing else can say
   whether the thing it names is still there — a removed flow is freed, so the next read is of freed memory. */
int   flow_is_member(const Flow *f);
/* The i'th flow in registry order, or NULL past the end — a WALK over the frontier's members, which is what a
   register living on the flows needs. flow_best answers which one to RUN; this answers who exists. */
Flow *flow_at(int i);

/* HOW MANY QUEUED PROGRAMS THE WHOLE FRONTIER STILL HOLDS FOR ONE DOCUMENT — the count over every member's
 * `dyn_doc` column, summed.
 *
 * IT EXISTS BECAUSE THAT COLUMN IS THE ONE HOLDER OF A DOCUMENT THE COLLECTOR CANNOT SEE. Every other way a
 * flow names a realm is a counted JSValue — its suspended frames, its jobs, its parked continuation, the dups
 * inside its COW delta — so a realm a flow can resume into cannot be collected, which is what makes reclamation
 * safe at all. A `dyn_doc` entry is a uint32 HANDLE: it holds nothing, keeps nothing alive, and stays perfectly
 * readable after the realm behind it is gone. So it is exactly the state a realm's teardown has to be asserted
 * against, and core/frame/navigable.c asserts it at the one moment a realm dies.
 *
 * PURE: no allocation, no JS value touched, no reference taken — it is called from inside a collection (the
 * realm-teardown hook fires there), where allocating or dup'ing would re-enter the walk that is running. */
int   flow_programs_for_document(uint32_t doc);

/* HOW MANY PROGRAMS OF ONE DOCUMENT ONE FLOW STILL HAS QUEUED AND HAS NOT STARTED — the other question the
 * one above says it is not answering ("a caller that wants \"still to run\" would be asking a different
 * question with a different assert behind it"), and this is that caller.
 *
 * IT IS PER FLOW BECAUSE THE OPERATION THAT ASKS IT IS. HTML §7.5.10 "Destroying documents"' destroy a
 * Document is state a page OBSERVES, so it runs once per timeline over that timeline's own delta — at the
 * instant flow A destroys a document, flow B has not destroyed it and its rows for that document are rows of a
 * document that is still there. A sum over the frontier would call B's ordinary queue A's defect.
 *
 * AND IT IS UNSTARTED BECAUSE §7.5.10 STEP 7 IS. "Remove any tasks whose document is document from any task
 * queue (without running those tasks)" is about work that has not run; a row the flow has already compiled is
 * a program it may be SUSPENDED INSIDE, and the standard has no object at all for a continuation suspended
 * mid-program — that one is the flow's own state and §NO BOUNDS forbids touching it. `last_compiled` is the
 * line between them, because it is what the compile site advances and asserts against.
 *
 * PURE: no allocation, no JS value touched, no reference taken, so a DCHECK may ask it. */
int   flow_programs_unstarted_for_document(const Flow *f, uint32_t doc);

/* TAKE THEM OUT — HTML §7.5.10 "Destroying documents"' step 7, "Remove any tasks whose document is document
 * from any task queue (without running those tasks)", performed on the queue the runtime's own job walk cannot
 * see. Returns how many rows went, which is exactly what the count above answered one instant earlier.
 *
 * WHY THIS QUEUE IS ONE STEP 7 IS ABOUT. `JS_DropJobsForContext` empties the runtime's job queues, and a
 * document's SCRIPTS are not in them: they are rows of the running flow's one program sequence. A row is a task
 * by the standard's own reckoning — §8.1.4.4 "Calling scripts" runs it and §4.12.1.1 "Processing model" queues
 * it ("queue an element task on the DOM manipulation task source") — so a row of a destroyed Document left
 * behind is page script that will be compiled into a realm whose browsing context is null.
 *
 * IT IS THE RUNNING FLOW'S ROWS AND NOT THE FRONTIER'S, for the reason the count above is per flow: the
 * destruction is per timeline, and a sibling arm that has not destroyed this document is running a document
 * that is still there. Taking its rows would destroy something in a timeline that never asked.
 *
 * IT ALSO REPAIRS EVERY INDEX INTO THE SEQUENCE, which is the half a caller must not try to help with: the
 * pending register's `scriptI` and the flow's own `imm_at`/`imm_next` are absolute positions, and a compaction
 * that moved rows without them would deliver one document's script text into another document's row. `script_i`
 * and `last_compiled` are provably unaffected — see the note at the end of the body.
 *
 * NOT PURE: it frees rows and mutates the register, so unlike the two counts above it may NOT be asked from
 * inside a collection and may not stand in a DCHECK. */
int   flow_programs_remove_for_document(Flow *f, uint32_t doc);

#endif
