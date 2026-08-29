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

/* A NODE OF THE FORK TREE, AND THE ONLY THING IN A FLOW THAT OUTLIVES IT. It exists for one sentence: the thread
   time a DEPARTING flow burned has to reach the flow that FORKED it, because §scheduler prices the aging term
   against a monopolizer and a fork chain is one monopolizer wearing N names. OPAQUE here on purpose — a rank
   input has exactly one reader (flow.c's flow_weight), and a field nothing outside can spell is a field no other
   subsystem can grow a second ranking out of. See flow.c for the refcounting and the charge. */
typedef struct FlowAcct FlowAcct;

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
    double val;            /* accumulated emitted VALUE (new @H + @S) — the WFQ's reward term, ONE POINT PER
                              EMISSION (both detectors credit exactly 1.0) */
    /* HOW MUCH OF `val` THIS FLOW DID NOT EARN — the reward it INHERITED, at the instant it was forked or
       rebuilt. It ranks nothing and has exactly one reader, flow_wfq_census below, because `val` alone cannot
       answer the question a frontier that retires flows steadily without emitting anything poses: whether its
       members are producing and being outranked, or coasting on an ancestor's findings. `val` is copied at
       every fork (flow_fork_inherit) and restored by the cold tier, so it is monotone down a fork chain and
       says what an ANCESTRY emitted; `val - val_born` is what THIS flow emitted, and a frontier where that is
       zero for every member is one whose whole reward ordering was decided before any of them existed.
       A FROM-BASELINE FLOW INHERITS IT TOO, and this line used to say the opposite ("0 for a from-baseline
       flow … it inherited nothing, so all of its reward is its own from the start"). That was a description of
       a defect: `val` is the frontier's dominant term and a newcomer entering at zero entered BELOW every
       member of a frontier whose arms had all inherited the boot family's reward, which is not "the system's
       virtual time" and is not reachable by the ordering again. flow_arrive_at_virtual_time assigns both halves
       now, exactly as flow_fork_inherit does, so this field means the same thing at both doors: everything a
       flow was handed, and `val - val_born` is everything it has emitted since. The cold tier's rebuild is the
       one writer that REPLACES the pair rather than inheriting it — a resumed flow is a returning member
       carrying its own parked account, not a newcomer being placed. */
    double val_born;
    /* THE AGING TERM, AND ITS UNIT IS THE WHOLE OF WHETHER THE TERM WORKS. Thread time in MICROSECONDS burned
       since this flow's last emit — never a step/opcode/visit count. A count is not commensurate with `val`
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
       one is the aging's own half, and the family's is the other. Both reset at flow_credit_emit, which is what
       makes them AGING (silence) rather than lifetime service.
       WHY BOTH HALVES AND NOT ONE. The family charge alone cannot order anything WITHIN a family, because every
       arm reads the identical number — and a real page's whole frontier is one family (every flow descends from
       the boot flow through flow_fork_inherit), so §scheduler's "a monopolizer that burns CPU without emitting
       sinks below productive+unrun flows" was a statement with no comparison left in it exactly where the
       monopolizer is. The own half is what makes it true among siblings; the family half is what keeps a fork
       chain one accounting unit against OTHER families (see `family` below, and the 8910-against-1124 reading
       that put it there). Neither replaces the other. */
    int64_t cpu;
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
       INHERITED AT A FORK for the same reason `val` and `cpu` are — an arm has, by construction, completed every
       unit its parent completed before the branch — and RESET BY AN EMISSION for the same reason `cpu` is: a
       flow that just produced something is not one the frontier needs protecting from. flow_fork_inherit's
       rank-neutrality DCHECK is what forces both, and it fires the moment either is forgotten. */
    int64_t visits;
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
       SO THIS IS NEVER ACCUMULATED AND NEVER PAID. It is the best fraction of THIS flow's own payload that any
       re-execution has been observed to deliver, in [0,1], overwritten upward and read at the pick. It cannot
       double-count because there is nothing to count: two flows at the same distance simply compare equal, and
       one that falls behind another is passed. It shares the optimism term's entire range, so it is priced
       against the same aging and buys a candidate the same order of thread time a never-run flow gets.
       IT IS CARRIED BY A FORK for the reason every weight term is: an arm of a candidate is that candidate's
       run continued under one more arm, carrying the same payload to the same sink, so a fork that dropped it
       would let a candidate improve its own rank by branching. flow_fork_inherit's rank-neutrality assertion is
       what forces that and fires the moment it is forgotten.
       IT DOES NOT CROSS THE COLD TIER, and that is the same decision `cand_fired` records one field up: a
       distance is an OBSERVATION of a re-execution, and a resumed session has not made it. A parked candidate
       comes back at zero and re-earns it from its first arrival, which is what keeps the number a measurement
       of this session's runs rather than a rank inherited from a run nobody watched. */
    double cand_dist;

    /* IS THIS FLOW A DRIVEN ORPHAN — a flow whose frame is a CALL of a function the page defined and nothing
       ever called (engine.c's engine_orphan_fork). It is not a different KIND of flow in any way the scheduler
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
       url". NULL for an INLINE row, whose base URL §4.12.1.1 states as "el's node document's document base
       URL" and which the compile therefore reads from the document instead.
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
       its last one — and a TASK runs when the sequence is exhausted (engine.c's flow_checkpoint_due). One array
       keeps both in a single arrival order — which is what a task source needs among its own tasks — and the
       pick (flow_job_take) applies the checkpoint rule.
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

/* THIS FLOW COMPLETED A UNIT OF WORK — the optimism term's "visit", credited by the scheduler at the ONE point
 * that can see the whole of a step: after flow_step returns, when the flow is left BETWEEN units. It is a
 * scheduler statement rather than a flow_step one because flow_step returns from a dozen arms and half of them
 * leave the flow suspended in the middle of a program, which is not a completed trial; the caller already
 * computes that predicate for HTML §8.1.7.3 "Processing model"'s end-of-checkpoint steps and this is the same
 * boundary. Asserted at its site: a flow inside a program may not be credited one, because the whole reason the
 * term is a unit count is that thread time inside a program is exactly what it must NOT measure. */
void flow_credit_visit(Flow *f);

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
 * `val_zero` IS THE ROW THAT NAMES A POPULATION rather than a statistic. A from-baseline flow enters at reward
 * 0 (flow_add's zeros) — a candidate session, a joined document's boot flow, the first flow — so its weight is
 * at most 1.0 for its whole life, and a candidate records no endpoints by design (endpoint_suppress) so the
 * only thing that can raise its reward is the very sink it is trying to reach. Beside `val_max` the pair says
 * exactly where those members sit in the order.
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
    double val_min;    /* the reward term's range over the frontier. A spread above 1.0 is the statement that */
    double val_max;    /* the optimism bonus can no longer reorder its ends: only aging reaches the bottom. */
    double val_top;    /* …and flow_best's own reward, so the top of the order is named rather than inferred */
    long val_zero;     /* members that inherited nothing and have emitted nothing — ceiling 1.0 (see above) */
    long self_emit;    /* members with val > val_born: they emitted something THEMSELVES rather than inheriting
                          it. Zero here while `finished` climbs is work that advances no statement. */
    long unrun;        /* members with cpu == 0 — never charged for the thread, or emitted since they last
                          were. IT IS NOT flow_pick'S OWN DEFINITION, which is what this row used to claim: the
                          pick's `unrun` is the population whose weight is at least 1.0, and that needs every
                          term of the weight at zero (visit count, own service, family service), not one of
                          them. This is the broader set on purpose — read against `vis_max` below it separates
                          "nothing has been charged yet" from "nothing has FINISHED anything", which is the
                          distinction the pair exists to make — but a reader taking it for the assert's
                          population would be reading a superset. */
    int64_t svc_max;   /* the largest service notch in the frontier — who is actually consuming the thread */
    /* …AND THE OTHER END OF IT, WHICH IS THE ONLY NUMBER IN THIS STRUCT THAT CAN ANSWER "IS THE AGING TERM
       MEASURING THIS FLOW OR THE WHOLE FRONTIER". `svc_max` alone reads identically for a single monopolizer on
       an otherwise-idle frontier and for a frontier every member of which has burned the same thread time, and
       those take opposite actions: the first is the case the aging term was priced for, the second is one where
       every member is being charged for work the frontier as a whole did. The FLOOR is what separates them, and
       it is also the virtual time a from-baseline flow now arrives at (flow.c's flow_arrive_at_virtual_time), so
       `svc_max - svc_min` is the SPREAD of service the ranking is actually made of.
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
       reward SCALE was what a run was stuck on: `val` is copied at every fork while the aging meant to cancel
       it was charged to whichever arm held the thread, so a family with N live arms presented its reward N
       times and paid for it N times over. The reading came back 8910 against an `svc_max` of 1124 — a factor of
       7.93 — and flow_weight's aging term now reads this quantity instead (flow.c's FlowAcct `fam_us`).
       SO THE PAIR IS READ THE OTHER WAY ROUND FROM HERE ON. `svc_fam_max` is the AGING's own denominator and
       `svc_max` is one member's share of it; their ratio is the fork factor of the widest family in the
       frontier, which is a fact about the DOCUMENT's branching and no longer a defect in the ordering. What
       would be a defect is `svc_fam_max` sitting far below `svc_max`, which cannot happen while every arm's
       charge lands on the family — and flow_age_running asserts the invariant that makes it impossible. */
    int64_t svc_fam_max;

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
     * context-escaped, handler-fires} the WFQ reads". flow_weight reads none of those four — a candidate 800
     * gates into its runway ranks exactly like one 3 gates in. This is the number that says whether that gap
     * is what is costing the run, and it separates three states that look identical from outside: served and
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
     * like one. A candidate records no endpoints by design (endpoint_suppress) so its reward is 0 until it
     * fires, and flow_weight for reward 0 is `1/(1+v) - (s+F)*Q*RATE`. The arithmetic that stood here was
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
     * it CREATES above every member already waiting. What is still absolute is the aging term itself, and the
     * consequence is at LEVEL-1 rather than here: `engine_top_weight` is this function's value with no frame of
     * reference, so a document's best flow falls by one point per second of unproductive CPU without bound while
     * a document that boots today enters at 1.0. Nothing but an EMISSION ever raises a weight, and a document
     * that cannot win the Level-1 pick cannot emit, so the crossing at `(reward + 1)` seconds is a RATCHET: past
     * it a mature document is outranked by every page that arrives afterwards, permanently, for as long as the
     * pool keeps being fed. That is what `svc_min` above is for — the frame of reference, measured. */
    double w_top;         /* flow_best's weight — what holds the front of the queue */
    double w_min;         /* the lowest weight in the frontier — the other end of the same order */
    double cand_w_max;    /* the best weight any @S candidate can offer against `w_top` */
} WfqCensus;
void flow_wfq_census(WfqCensus *out);

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
 * the STALL, decided by asking each member rather than by counting a run of unproductive picks. */
Flow *flow_next_to_run(const Flow *incumbent);

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
/* THE RUNNING CANDIDATE'S OWN BYTES WERE OBSERVED THIS FAR ALONG — §@S's fitness written where a fitness goes.
   `d` is the fraction of this flow's payload a re-execution delivered, in [0,1]. It RAISES `cand_dist` and does
   nothing when the flow already stands further along, so the quantity is monotone per flow and an observation
   can never demote the flow that made it. Not a credit: nothing is added, nothing accumulates, and this may be
   called for the same distance any number of times. It bumps the frontier generation exactly as an emission
   does, because a weight that moves without one is a rank the value-yield cannot see changing. */
void  flow_set_distance(Flow *f, double d);
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

#endif
