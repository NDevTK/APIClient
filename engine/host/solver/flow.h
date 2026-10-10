/* Flow and WFQ: the scheduler's unit of work and the order it is served in.
 *
 * A flow is a code flow through the program: a decision vector over the shared pre-boot baseline, so its state
 * is replay(baseline, decision vector). A fork appends an arm to a decision vector, which is the same operation
 * whether the branch was a bytecode OP_if or a native builtin loop-back, so a flow is (fn, decision vector) and
 * frame-agnostic by construction.
 *
 * The WFQ orders flows by an anytime-bandit priority: accumulated emitted value, plus a UCB optimism bonus
 * proportional to 1/(1 + visits) so a never-run flow is never starved, minus CPU aging so a flow that burns CPU
 * without emitting sinks below productive and unrun flows. It orders only and never drops a work item. A
 * monopolizer is a fork chain rather than one flow, so the reward and the family half of the aging live on the
 * fork family's account (`family` below, flow.c's FlowAcct). */
#ifndef ENGINE_HOST_SOLVER_FLOW_H
#define ENGINE_HOST_SOLVER_FLOW_H

#include <stdint.h>   /* int64_t: lifetime counts are 64-bit because `long` is 32 bits in wasm */
#include <lexbor/dom/dom.h>

#include "quickjs.h"
#include "solver/world.h"
#include "solver/pending.h"   /* the replies the host still owes a flow (a JS Array, not a malloc'd list) */
#include "solver/dyn_body.h"  /* the program text a flow's sequence holds; shared, because no flow writes it */
#include "solver/step_unit.h" /* the named arm of flow_step a flow last returned through */

/* A node of the fork tree, and the only part of a flow that outlives it: the thread time a departing flow burned
   must reach the flow that forked it, because the aging term prices a fork chain as one monopolizer. Opaque so
   that flow.c's flow_weight is its only rank reader. Refcounting and the charge are in flow.c. */
typedef struct FlowAcct FlowAcct;

/* The @S fitness ladder; flow_distance composes the comparator from it, so the rung count is arithmetic.
   Rung 0 is the runway: the fraction of its recorded path a candidate has replayed, observed at dec_replay,
   strictly before the source read. Rung 1 is the survival fraction of the payload at a sink. Above them sit
   three booleans held as one count (Flow.cand_rung): delivered (substituted at a source read), then arrived and
   escaped (both observed at a sink write).
   The fire is not a rung: it is the outcome, a finding paid by the ledger, and the comparator orders the
   candidates that have not fired.
   At the delivery the runway fraction is pinned to 1.0 by definition, so the bands are disjoint and ordered:
   undelivered [0, 0.2], delivered [0.4, 0.6], arrived [0.6, 0.8], escaped [0.8, 1.0]. A survival fraction below
   the delivery is unreachable, because survival is measured only on delivered bytes; flow_observe_survival
   asserts it. The range is exactly 1.0, which prices the distance against the optimism term so a promise never
   outweighs a finding; flow.c's flow_silence_us_to_sink reads FLOW_RUNGS_N rather than a literal. */
#define FLOW_RUNG_DELIVERED 1 /* this flow's payload was substituted into the page's own program (a source read) */
#define FLOW_RUNG_ARRIVED   2 /* this flow's breakout reached the sink its own search is for */
#define FLOW_RUNG_ESCAPED   3 /* …and stood there in a position the sink's own language executes */
#define FLOW_RUNGS_N        5 /* the two fractional rungs plus the three above: the comparator's denominator */

/* What one step of a flow answered. OWED is the same flow reporting that the work it has left belongs to the
   host, so the scheduler can tell an exhausted frontier from a waiting one without any member leaving. */
#define FLOW_STEP_MORE  0
#define FLOW_STEP_DONE  1
#define FLOW_STEP_OWED  2

typedef struct Flow {
    /* This flow's slot in the frontier's array, so locating a member is a load rather than a walk. It names a
       slot, never a rank: the array is in arrival order and the order is by weight, so no term of flow_weight
       may read it. Exactly two writers keep it exact: flow_new's append and flow_remove's swap-remove, which
       re-keys only the member moved from the end into the vacated slot. No Flow is ever copied, and the
       registry's realloc moves the pointer array, not members. flow_pick asserts the field against its index at
       every member of every scan; flow_is_member cannot use it, because it must answer for a dangling pointer. */
    int reg_i;
    /* This flow's world: its name in the one timeline it owns, valid in every document it touches
       (solver/world.h). `delta` is only this instance's segment of that world; a flow that scripts an iframe or
       a popup writes in another WASM instance, which keys its segment by this id, because a delta names live
       heap pointers and cannot travel. It changes at every branch: a fork retires the world it branched at and
       mints a child for both arms, so a world a peer holds a segment for is never written again. */
    WorldId world;
    JSValue fn;            /* the function this flow re-drives (JS_UNDEFINED for a boot/session flow) */
    /* The decision vector is not a field here. It is `dec_blob` below, the shared frozen chain, whether it was
       frozen by a suspend or a fork or rebuilt by the cold tier from a recipe, so a resumed flow and a forked
       one are the same kind of thing downstream. A flat per-flow array would multiply the sharing back out. */
    /* What this member has emitted (new @H and @S), one point per emission. A census quantity, never a rank: the
       WFQ's reward is the fork family's (FlowAcct `val`, read through flow_reward), kept at the same accounting
       unit as the aging that cancels it. This field says which members are producing.
       Never copied, never inherited, never read by flow_weight; flow_fork_inherit asserts it is still zero,
       because a value copied at a fork differs between two arms by birth order rather than merit. */
    double val;
    /* The own half of the aging term: thread time in microseconds this flow burned since its fork family last
       emitted, never a step or opcode count (a count is not commensurate with the reward). The exchange rate is
       FLOW_AGE_RATE and the term steps in flow.c's FLOW_AGE_QUANTUM. int64_t because a 32-bit wasm `long`
       overflows after about 2147 s, and a negative value would rank a monopolizer first.
       It is a reading only while `cpu_gen` matches the account's `emit_gen`, so every reader goes through
       flow.c's flow_own_silence and never reads it raw. Written only for the flow holding the thread; inherited
       at a fork. The family half (`family`) keeps a fork chain one unit against other families; this half
       orders arms within one family, where every arm reads the same family charge. Both reset at an emission
       credited to the account, which makes them silence rather than lifetime service. */
    int64_t cpu;
    /* Which silence window `cpu` is a reading of: this flow's copy of its account's `emit_gen`. An emission
       forgives the whole family with one increment of `emit_gen` rather than a walk, because flow_weight is O(1)
       and is evaluated inside DCHECK conditions. Inherited at a fork together with `cpu`; flow_fork_inherit's
       rank-neutrality equality fails if only one is copied. flow.c's flow_age_running normalises a stale value
       where the flow is charged. */
    uint64_t cpu_gen;
    /* The optimism term's quantity: units of work completed, not a clock. flow_credit_visit, its only raiser,
       credits one when a step leaves the flow between units, holding neither a program nor a parked
       continuation: the "JavaScript execution context stack is now empty" point of HTML §8.1.4.4 "Calling
       scripts", where the microtask checkpoint also sits. Keyed on units rather than thread time, a flow keeps
       the thread until its program ends, so the jobs it queued (reachable only with `frame == NULL`) run.
       Inherited at a fork, since an arm has completed every unit its parent did. Not copied at an arrival
       (flow_add_unseeded): an arrival stands on nobody's decisions and starts at zero, the value the never-starved
       guarantee is about; flow.c's flow_optimism keeps this term outside the arrival's coordinate.
       Never reset, including by an emission: a zero written on an emitter would be copied by its later forks and
       split two arms of one parent by an event neither performed. A park does not carry it; a cold-resumed
       member re-enters at zero and re-earns it by replaying. */
    int64_t visits;
    /* How many times the scheduler has handed this member the thread. A census quantity, never a rank: not read
       by flow_weight, not inherited at a fork (flow_fork_inherit asserts it is zero, which also keeps it out of
       the weight), never reset. It names the starved population, which no weight term can, because an emission
       resets the silence half of every term. It separates two diagnoses that take opposite work: a world missing
       because its flow was never picked is an ordering defect, while one whose flow was picked and made no
       progress is a resume-seam defect (`vis_zero` counts members that finished nothing, which covers both). */
    int64_t picks;
    /* The family half of the aging term: a direct pointer at the root of this flow's fork family, shared by every
       arm, holding the thread time the family burned since any arm last emitted. A from-baseline flow founds one
       (its own `acct`); a fork joins its parent's (flow_fork_inherit), so a reward stated once per family is
       cancelled by aging charged once per family. It is not the ancestry (`acct` is); it is direct so
       flow_weight costs one indirection, as it must inside DCHECK conditions. */
    FlowAcct *family;
    /* This flow's node in the fork tree: minted with the flow, attached under its parent's by flow_fork_inherit,
       and refcounted so `family` stays addressable while any descendant can read it. See flow.c. */
    FlowAcct *acct;

    /* Interleaving state, kept while this flow is paused so the scheduler can run another flow and come back. A
       flow is preempted mid-execution (cooperative quantum) and resumed byte-identically; its COW delta,
       decision cursor and pins swap with it (engine.c). Zero-initialized by flow_add. */
    /* A candidate session: this flow re-runs the page with one attacker payload substituted for one source, to
       see whether it fires at the sink. It is an ordinary flow (same scripts, scheduler and preemption) that
       carries the substitution. NULL for an ordinary flow. */
    char *cand_src;        /* the source identity the payload replaces (owned) */
    char *cand_payload;    /* the breakout to try (owned) */
    const char *cand_sink; /* the sink name to record if it fires (static) */
    /* Per-flow because a candidate is preempted and resumed among other flows, so a global would record another
       flow's marker or leave the substitution live for the next one. They swap with the flow. */
    /* Neither crosses the cold tier. solve_flow_begin re-derives `cand_verifying` from `cand_src` at every
       switch-in. `cand_fired` is dropped on purpose: a candidate can fire and then park before it finishes, and
       only a fire the resuming session observes proves a PoC. See cold.c's park_rec_cand. */
    int cand_fired;        /* this flow's X9 marker executed */
    int cand_verifying;    /* this flow is a candidate run: the sink takes the concrete arg */
    /* Where this candidate's payload came from: a park record it was rebuilt from (1), or this session's search
       list (0). solve.c's arrival check reads it to tell a resumed candidate, whose payload legitimately has no
       row in this session's list, from one assembled outside both doors. Set only by the cold tier's rebuild
       arm, never parked (a re-park writes a 'c' record and comes back through the same arm); carried by a fork. */
    int cand_resumed;      /* this candidate was rebuilt from a park record, so its payload has no row here */
    /* How far this candidate's own bytes have got: the @S fitness read as a comparator, a different quantity from
       the reward. A reward is a ledger, paid at most once per observation; a fitness states where an item stands
       now and must be readable off every item, including ones standing where an earlier item stood. So the
       distance is never accumulated and never paid: each rung below is overwritten upward, flow_distance
       composes them in [0,1], and the pick reads it. It shares the optimism term's range, so it is priced against
       the same aging.
       The fire is not a rung (FLOW_RUNG_* says why). Carried by a fork, because an arm of a candidate continues
       the same payload to the same sink and must not improve its rank by branching; flow_fork_inherit's
       rank-neutrality assertion over the whole weight enforces it. It does not cross the cold tier: a distance
       is an observation of a re-execution, so a parked candidate comes back at zero and re-earns it. */
    /* Rung 0: the fraction of its own recorded path this candidate has replayed, in [0,1], the only rung observed
       strictly before the source read. It counts replayed arms, not the raw cursor, because `g_c` also advances
       on appends; dec_replay samples it only past the key comparison, where the branch re-asked the recorded
       question. A divergence ends the vector at the cursor and dec_replay asserts `g_c < dec_total()`, so no
       reading is taken after a truncation.
       Monotone within a run: the recorded path is fixed, so a lower reading is another sample, not a demotion.
       Pinned to 1.0 at the delivery by flow_observe_rung. Not parked. */
    double cand_replay;
    /* The two halves `cand_replay` was computed from, for the report: its thousandths round to 0 on a long path
       with few arms consumed, so the report needs the raw counts. Written at the one sample that set the
       fraction, so `cand_replay == arms/of` exactly; flow_observe_replay asserts it, and `of == 0` if and only if
       `cand_replay == 0.0`. Not pinned at the delivery (the pin is a definition, not an observation), so past the
       delivery the pair says what was last observed while the fraction says where the comparator stands.
       Not a weight term: flow_distance reads only `cand_replay + cand_surv + cand_rung`. Carried by a fork anyway,
       because a sibling holding the fraction without the pair would fail the identity at its next replayed arm.
       It passes the two-instants test by construction: a flow that forks has stopped replaying (decide.c forks
       only past the recorded path), so a parent consumes no arm between two of its forks. Not parked. */
    long cand_replay_arms;   /* `consumed` at the sample that set `cand_replay` */
    long cand_replay_of;     /* `total` at that same sample; 0 = no reading has ever been taken */
    /* Rung 1: the best fraction of this flow's own payload any re-execution has delivered to a code-execution
       sink, in [0,1]. A fraction because how much of the payload survives has degrees. */
    double cand_surv;
    /* The boolean rungs as one count, so their order is arithmetic: 0 = this flow's bytes are not in the program,
       FLOW_RUNG_DELIVERED = substituted at a source read, FLOW_RUNG_ARRIVED = the breakout reached the sink its
       search is for, FLOW_RUNG_ESCAPED = it stood in an executable position there. A flow cannot hold a rung
       without its predecessor (each escape site in solve.c runs downstream of the arrival site on the same
       string, and a candidate arm returns at the sink's door unless its substitution happened);
       flow_observe_rung asserts it. 0 is a positive statement: it separates a flow never served, or killed on the
       runway, from one whose bytes are in the program and were eaten by a filter. */
    int cand_rung;

    /* Is this flow a driven orphan: one whose frame is a call of a function the page defined and nothing called
       (engine.c's engine_orphan_seed)? To the scheduler it is an ordinary flow. Its one consequence is that its
       work is not in its recipe: re-running the document never calls the function, so the recipe also carries a
       function locator (`orphan_hash`) and a resumed flow drives the body it names. Inherited by a fork. */
    int orphan;
    /* Where that function is, in a form a later session can find: quickjs's JS_OrphanHash of the script the body
       was compiled from, its position in that script and its own source text. `fn` dies with the session; this
       crosses the tier, stamped when the drive is created and carried unchanged by every arm. 0 for a flow that
       is not a driven orphan, which the park asserts. */
    uint64_t orphan_hash;
    /* Is this drive still waiting for its function? Set only by the cold tier's rebuild. A resumed session's
       heap holds the closure only once the document's own replay creates it and some flow takes it as an
       orphan; until then this flow replays the document like any other. `fn` tells the two states apart:
       undefined while waiting, the re-taken function once handed one. The flow then builds its own call frame in
       its own timeline, because the receiver and arguments are concolic objects owned by the minting flow. */
    int orphan_want;
    /* The callee's declared formal parameter count, handed over by the take beside the function. Live state, not
       part of the recipe, since it is a fact about the body this session compiled. Meaningful only while
       `orphan_want` is set and `fn` is a function. */
    int orphan_argc;

    /* Has this flow's recipe been written to the park document? A per-flow fact because a partial self-park
       writes and releases the lowest-value tail while the engine keeps running. flow_release reads it (a
       continuation on a written flow is replayed next session; one on an unwritten flow is dropped), and
       cold_park_flow sets it and refuses a flow that already carries it. */
    int paged;

    /* Has this flow told the scheduler it can make no progress? A flow answers FLOW_STEP_OWED when all it has
       left belongs to the host (an unanswered fetch, a document script whose text has not arrived, an
       unresolved synchronous cross-instance read), and it must not get the thread again until something could
       have changed. Recorded per flow, because the WFQ re-picks the same top flow and a count of consecutive owed
       answers would declare the frontier stalled while runnable siblings were never asked.
       A generation stamp, not a flag, so clearing every mark is one increment (flow_clear_host_owed). */
    unsigned owed_gen;

    /* Does this member owe the host an image of its own world? Each flow is a distinct appearance of the
       document, and its pixels exist only while it is switched in with its deltas applied, so the host asks for
       the moment instead of rendering whichever timeline is standing at a slice boundary (main.c's `qjs_paint`).
       A plain flag, unlike `owed_gen`: a generation would read every member born after the stamp moved as
       marked, while this mark must be laid deliberately, by the frontier walk or by the mint. With the
       every-world mode on, flow_new marks a newborn; a fork never inherits it (`reclaim_calloc` zeroes a Flow).
       The marked set grows with the frontier, which is the point; nothing waits for an ask to close, since a
       mark is spent at its member's end or at any yield it stands for, so the cost is one return per member.
       It decides nothing about the order: the scheduler hands the thread back only when its own pick has put
       that member in front, so it cannot forge a ranking record (flow_switch_in writes one). */
    int   paint_owed;

    /* Has this flow a recorded path to stand on? 0 = fresh: decide_enter gives it an empty vector and every
       branch is a new decision. 1 = it resumes from the blobs below: either a snapshot-forked sibling (a live
       frame plus its chain) or a flow the cold tier rebuilt from a recipe (no frame, cursor 0, replaying its
       arms from the first script). They are one state on purpose. */
    int   started;
    /* Which arm of flow_step this member last returned through (solver/step_unit.h owns the list). The scheduler
       stamps it at the one point every step converges on. A pure record, read by the `@COLD` histogram and the
       seamless-stretch aborts; `STEP_UNIT_NONE` on a calloc'd member, the true answer for a flow never stepped.
       Not parked: it describes a step this session took. */
    StepUnit step_unit;
    /* This flow's live preemptible base, NULL when it has none. It holds one of three kinds: a program of the row
     * at `script_i` (JS_FlowNew), a call this host built (JS_FlowNewCall: a driven orphan's, an HTML §8.1.4.4
     * step 8 report, a §6.10.1 task), or a clone of an activation this host never created (JS_FlowClone at a
     * concolic branch inside a module body). Ask the kind with JS_FlowIsProgram / JS_FlowIsCall, two
     * predicates over one `base_kind` field; never infer it from this slot being occupied. */
    void *frame;
    /* This flow's program still being parsed, NULL when there is none (quickjs.h's JS_FlowCompileStep and
     * JS_FlowEvalModule take it). The compile of the row at `script_i` is O(a length the page chose), so it
     * suspends, and a parse that gave the thread back waits here. It is not a second frame: while it stands the
     * row's program has not started, `frame` is NULL and the cursor has not moved.
     * Not cold-tier state: the suspended parse is a graph of raw pointers the cold tier cannot serialize, and a
     * recipe replays the document, which re-compiles; flow_release frees it. A fork never carries one, because a
     * branch is taken by running bytecode; asserted where a sibling is built.
     * One slot for both §8.1.4.4 "Calling scripts" entries, since a flow holds at most one row's parse; which
     * entry parked it is read off the step arm (two rows of solver/step_unit.h's list). */
    void *compile;
    /* Is that frame the row's program, or the report the row's program owes? HTML §8.1.4.4 "Calling scripts",
     * run a classic script step 8's third bullet reports an abrupt completion before step 8.3.2's clean up, so
     * the report is the flow's next work and takes the vacated frame slot as a continuation of the same row.
     * While set, `script_i` still names the script that threw, and the program's completion belongs to the
     * report's: the cursor advances once (advancing at the throw would skip the next script), §4.12.1.1
     * "Processing model" restores `document.currentScript` after the report (an `error` listener reads the
     * throwing element), and a report frame that throws is an engine defect (§8.1.4.6 step 6's
     * error-reporting-mode flag). JS_FlowIsCall cannot answer it, since a driven orphan also holds a call root.
     * Carried by a fork; not cold-tier state (a replay throws and owes the report again). */
    int   reporting;
    /* Is this flow's live frame a modelled close request's task (HTML §6.10.1 "Close requests")? Asked for
     * `reporting`'s reason: the frame is a call root, so JS_FlowIsCall cannot tell it from an orphan's call or a
     * report. Its throw is an engine defect (a close action never throws, and DOM §2.9's inner invoke step 2.11
     * catches a listener's), and its normal completion is a value that has to be read.
     * Carried by a fork; not cold-tier state (a replay re-reaches its own exhaustion). */
    int   close_req;
    /* Whether a modelled close request in this timeline reached §6.10.1's step 9, "Otherwise, there was nothing
     * watching for a close request": the standard's statement that this flow's document has nothing for a close
     * request to do, which lets the arrival stop being modelled without a counter. It is not a bound: the page
     * may establish a watcher later, and a flow that closed something is asked again (§6.10.2's process close
     * watchers takes one group at a time). Written from the task's completion value only, which is why
     * `close_req` exists. Carried by a fork; not cold-tier state (the manager is per-flow COW state). */
    int   close_req_none;
    /* Has this flow's path rested on something nothing observed? It is the derived/forced discriminator a
     * request this flow builds must declare: the value's bytes are the same either way, so the fact is recorded
     * where it happens. It is the contradiction, not the fork: forced execution forks both arms of nearly every
     * branch, while this marks an arm the concrete example disagrees with (an example-less branch marks nothing).
     * One writer, flow_mark_forced_arm, reached by three acts:
     *   - a branch whose arm the concrete example contradicts (solver/decide.c);
     *   - a request the trusted zone declined, whose failure arm runs the page's `catch` over an outcome nobody
     *     sent (solver/engine.c's flow_decline_fork);
     *   - a modelled potential close request, an arrival no user performed (solver/engine.c's close-request arm).
     * A new producer is a line in this list, not a second bit.
     * Monotone, since a path cannot un-take an arm, and never a weight term. Carried by a fork (flow_fork_inherit
     * asserts it). Not cold-tier state: a resumed flow re-observes each contradiction against current examples
     * as its replay re-reaches it. */
    int   path_forced;
    /* Whether this path pinned a value on one of those arms: a strictly narrower fact answering a different
     * question. `path_forced` says what a reply is worth; this says whether an address composed afterwards may
     * rest on bytes this engine chose, which decides whether fetching it may be spent. Set where decide.c pins a
     * source (concretize-on-pin) on the holding arm of an equality the flow's concrete example contradicts.
     * It cannot be read off the address: `pin_mint` (solver/concolic.c) returns a bare primitive and
     * `concolic_add_hook` returns 0 when neither operand is concolic, so a URL built from a pinned value is a
     * plain string; recovering the link would need a taint tracker. Strictly nested inside `path_forced` (its
     * writer runs only where decide_note_forced_arm has just marked the path); pending_prov_compose asserts it
     * at the park. Monotone, fork-carried, never a weight term.
     * Named residual. Not covered: decide_note_forced_arm returns early on REAL_ARM_UNOBSERVED, so a pin over a
     * source this run never observed leaves both bits clear. Next diff builds: a third state on this field and
     * the arm of decide_real_arm that answers it (it already reads concolic_example_state). Absence shows as: a
     * request graded forced by a later unrelated gate, with this bit clear, firing an address that holds the
     * earlier witness. */
    int   path_pinned;
    /* The document's load stage is not here: a flow reaches several Documents in its agent cluster and each has
       its own readiness, so it lives on each Document (document.c's readiness slot), isolated per flow by the
       COW delta. */
    int   script_i;        /* position in this flow's ONE program sequence: a row of `dyn`, on [0, dyn_n) */
    /* One sequence and one address space: the document's own scripts are seeded as rows of `dyn` at creation
       (flow_set_seed_hook), so HTML §4.12.1.1's "immediately execute the script element" is expressible at
       every position. A position may be a script whose source has not arrived (engine.c's DYN_SCRIPT_SRC row
       holding the address), and the flow stops at it, which gives every document §4.12.1.1's order rather than
       reply order; one host fetch answers every flow parked on the address (engine_provide).
       Stopping at a row does not start its fetch: §4.12.1.1 "Processing model" step 33 fetches when the element
       is prepared, and step 35 only decides where the result executes. Every row owes its request from creation
       (engine.c's engine_queue_into); the cursor decides only which program runs. */
    /* The highest script index this flow has compiled, so that compiling one twice can be caught. A flow runs
       each program in its sequence once and a preempted flow resumes its suspended frame; recompiling would
       re-execute side effects against a delta that already holds them. */
    int   last_compiled;   /* -1 until the flow compiles its first program */
    /* Where a run of interposed programs has reached, so a second interposition at one slot goes behind the
     * first. HTML §4.12.1.1 "Processing model"'s "prepare the script element" ends "Otherwise, immediately
     * execute the script element el, even if other scripts are already executing", so two elements one program
     * prepares run in the order prepared; expressing "inside" as the slot after the cursor alone would reverse
     * them (both compute one slot, and the second shifts the first down).
     * The witness is the base slot, not the cursor: `script_i + 1` inside a program and `script_i` between
     * programs, so the row and a job running before it have different bases and a stale pair is reused only by
     * a program whose interpositions belong behind it. No invalidation site is needed.
     * "Inside a program" is the frame's kind (JS_FlowIsProgram), not the slot being occupied: a call or a module
     * clone is between programs and stands at the end of the sequence. A per-row column cannot replace this,
     * since a row an ancestor interposed also carries DYN_POS_IMMEDIATE. A fork carries it. */
    int   imm_at;          /* the base slot `imm_next` was computed from; -1 while no run is open */
    int   imm_next;        /* the slot the next IMMEDIATE row of that run takes */
    /* This flow's own program rows (a lazily loaded chunk, a queued document script), or, while the row's kind
       is DYN_SCRIPT_SRC, the address of a script whose source has not arrived: one column, because it is one
       queue. The row is this flow's; the bytes are not. Source text is fixed once decoded, so every timeline
       holding a program holds the same buffer (solver/dyn_body.h) and a fork costs O(rows), not O(script
       bytes). The parallel `dyn_*` columns below are allocated, copied and freed together, so a field added to
       the queue is one obligation at every clone, free and finish site. */
    DynBody **dyn; int dyn_n, dyn_cap;
    /* Which document each program belongs to, which is where it is compiled: a program closes over the
       compiling realm's global (JS_FlowNew), and in an origin-keyed agent cluster a same-origin child's classic
       scripts are the creating flow's next programs (§7.4 step 14). A document handle rather than a JSContext*,
       because a queued program outlives the turn that queued it and a handle survives a park. It is also where
       a cross-agent operation's document lives. */
    uint32_t *dyn_doc;
    /* Each row's name: the one fact about a row that a shift cannot change. §4.12.1.1's immediate execution
       interposes rows below the cursor and §7.5.10's destroy removes them, so anything outside this table names
       a row by this id, never by position (flow_dyn_row_by_id searches by value). Minted from one global clock
       and never reused, because sibling arms mint independently and a register entry a fork shares
       (solver/pending.h) must resolve to the same row in each. A fork copies it. Not parked: a resumed flow
       replays its document and its rows take fresh names. Ids of one flow's rows are not contiguous. The row's
       arrival stamp is `dyn_run`, not this: the name is fixed at creation and may never move. */
    uint64_t *dyn_id;
    /* When the row became a runnable work item: the number flow_step's ladder hands flow_task_precedes. A
       script element's place in the document is fixed at parse, and its place in the event loop when its
       bytes made it ready. HTML §13.2.6.4.8 The "text" insertion mode blocks the tokenizer and spins the event
       loop, and §8.1.7.3 "Processing model" resumes the parse with a queued task, so a task queued while the
       bytes were in flight runs before the script. A separate column because `dyn_id` is an identity the
       pending register names rows by (PEND_SCRIPT_ROW).
       Two writers: engine_queue_into mints it equal to `dyn_id`, and the two lines that take a row out of
       DYN_SCRIPT_SRC (flow_deliver_one_reply's program arm and its null arm, §4.12.1.1 step 4's `error`)
       re-mint it. It is written once per readiness and never moved, so the set of items outranking a job is
       fixed at the job's birth: an order, not a bound. Zero is no stamp (flow.c's g_work_seq starts at 1), which
       lets the ladder DCHECK a live row's stamp. Not parked, for `dyn_id`'s reason. */
    uint64_t *dyn_run;
    /* The rendezvous token of the peer parked on the row, for the one kind of row that owes an answer: a
       cross-agent operation's answer is its program's completion, so the token belongs to the row. Per row
       because a second operation may arrive before the first answers, and two operations differing only in the
       asking world are otherwise indistinguishable. NULL for every other kind and again once the answer is
       sent; a DYN_CROSS_AGENT_OP row without a token is a peer suspended forever, so the kind and the token are
       written together at the one queue entry allowed to create that kind. */
    char **dyn_token;
    /* What kind each program is (a DynKind, engine.c). A page script that does not compile asserts; the other
       kinds are ordinary when they do not. Most @S candidates do not compile in most sink contexts, which is a
       parked search rather than a @WHY, and a `javascript:` URL that does not compile is HTML §7.4.2.3.2's
       abrupt evaluation, which produces no Document. A separate column so the page-script assert stays armed
       inside a candidate flow, which still loads real chunks. */
    unsigned char *dyn_cand;
    /* Which of HTML §8.1.4.4 "Calling scripts"'s two algorithms runs the row (a ScriptType,
       core/loader/document_scripts.h): §4.12.1.1 "Processing model"'s "execute the script element" switches on
       the element's type. Classic is a statement about a row, not a default: a `setTimeout` string, a
       `javascript:` URL, a lazy chunk and a cross-agent operation's program are classic scripts. Only a row an
       element put there can say module, so engine_queue_element_script and engine_queue_docscript_url are the
       only entry points with the parameter. */
    unsigned char *dyn_type;
    /* The address the row's bytes came from: HTML §8.1.4.2 "Fetching scripts" creates the script with the
       response's URL. NULL for an inline row, whose base URL is the element's node document's base URL
       (§4.12.1.1 "Processing model"), read from the document at compile. Not the body column: a DYN_SCRIPT_SRC
       row holds its URL in `dyn` only until flow_deliver_one_reply replaces it with the source, and the address
       is needed after that, to resolve a nested `import()` and, for a module, as the module map key. */
    char **dyn_url;
    /* The `script` element the row is the program of, or NULL for a row no element put there. §4.12.1.1
       "Processing model"'s "execute the script element" sets `currentScript` (§3.1.7) to that element for the
       whole run, and the run is a work item spanning scheduler steps, so the element travels with the row. NULL
       is the spec's own answer for a program no element caused (a string handler, a chunk reply, a
       `javascript:` URL, an @S candidate, a cross-agent operation).
       A borrowed node pointer that may never cross a park: a resumed flow replays its document and re-queues its
       rows. A snapshot fork copies it, sound because the sibling holds a reference on the node's DOM segment. */
    lxb_dom_element_t **dyn_el;
    /* Whether each row is a task or the synchronous tail of the program that caused it (a DynPos, engine.h). The
       microtask checkpoint is placed against it: HTML §8.1.4.4 "Calling scripts" performs the checkpoint when
       the execution context stack empties, and a DYN_POS_IMMEDIATE row (§4.12.1.1's "immediately execute")
       runs inside its causing program, so that program's checkpoint falls after the row; every other row is a
       task and the checkpoint falls before it. Position alone cannot say which: an immediate row and an
       appended one both land at the cursor when the queue was empty. */
    unsigned char *dyn_pos;
    void *delta;           /* this flow's isolated HEAP COW delta (CowDelta*), applied while running */
    void *dom; int dom_n, dom_cap;   /* this flow's isolated DOM COW delta head buffer (dom_cow), swapped with the
                                        heap delta at every context switch, so each flow sees its own document and a
                                        rewind restores it exactly. Detached via dom_buf_take while parked. */
    void *dom_base;        /* the shared immutable base-segment chain below the head (dom_cow_fork): a snapshot-
                              forked sibling references the parent's O(N) DOM delta in O(1). NULL until a fork. */
    void *dec_blob;        /* suspended decision state while paused (decide_suspend) */
    void *pin_blob;        /* suspended pin state while paused (concolic_pins_suspend) */
    /* This flow's own queued microtasks and tasks, run under its live COW so a reaction runs in the timeline that
       enqueued it. A microtask runs at the checkpoint HTML §8.1.4.4 "Calling scripts" owes once the program that
       queued it has left the stack, which is before the flow's next program. A task runs on any step that does
       not start a program of the sequence (flow_step's `else if (flow_job_pending(f) > 0)` arm, bound to
       `seq_compiles`), so a flow parked on an external script row runs this queue: §8.1.7.3 "Processing model"
       step 2's one task per iteration. One array keeps both in arrival order and flow_job_take applies the
       checkpoint rule (flow_checkpoint_due is the microtask arm).
       Not covered: §8.1.7.1 "Definitions" puts each task source in one queue, but the timer source is in both
       of a flow's carriers (a Function handler here via JS_EnqueueCallTask, a string handler in `dyn` via
       core/timing/timer.c), so the order between them partitions by carrier; engine.c's arm states the rest.
       A JS Array of immutable job records, so the leak walk sees the values, it parks and forks, and a fork
       shares entries at one refcount each. JS_UNDEFINED until the first enqueue, which keeps flow_job_pending a
       tag test at every suspend point. */
    JSValue jobs;
    /* This flow's own live fetches and the synchronous requests it is blocked on, resolved when its scripts and
       microtasks stall. A JS Array of plain records (solver/pending.h), because it must park to the cold tier,
       resume byte-identically and fork per flow. JS_UNDEFINED for a flow that never parked on anything. */
    JSValue pending;
    /* The parked continuations, swapped with everything else on a context switch. A forced preempt inside
       job-driven code parks a suspended async activation on the runtime's pump queue; it belongs to this flow's
       timeline and must resume under this flow's delta. NULL for a flow with nothing parked.
       One opaque handle for the whole set, because a step can park several bases (a settle nested inside a
       reaction; an async body completing while the reaction that resumed it is on the C stack) and each park's
       record lives on its base (quickjs's JSAsyncFunctionState), so what crosses the switch is their FIFO order.
       The flow owns the references: each continuation is kept alive by a reference its park took and only its
       resume returns, so a flow torn down or paged out returns them via JS_FreeParkedFlows at flow_release. */
    void *parked;
    /* Routed cross-document deliveries this timeline has been handed and not yet made: each the record the
       trusted zone routed here, paired with the sender's origin, which only that zone may stamp (SECURITY.md).
       A delivery is a work item on the one frontier, attached to every live flow of the receiving document,
       because the page's `message` listener lives in the delta of the flow that registered it; each flow
       delivers under its own delta when it next steps, onto its own task queue.
       A FIFO, not a slot: HTML §9.3.3 "Posting messages" queues a global task on the posted message task source,
       so two posts from one sender are two tasks observed in order. Senders whose worlds contradict may not
       share a timeline; that is a fork, asserted at the arrival (engine_route).
       A JS Array of immutable [record, senderOrigin] pairs, so it parks (cold.c writes 'm' records) and a fork
       shares the pairs at one refcount each. JS_UNDEFINED on a flow with nothing to deliver. */
    JSValue deliver_q;
    /* Which sending timelines this receiving timeline is in. A cross-document message carries the sending flow's
       world, and two arms of one sender branch carry contradicting vectors (world_vec_relate), so a timeline
       that received both would be one neither sender was in. Recording it on the timeline makes the verdict
       independent of whether the two arrive together or one after the other.
       Each entry is an immutable [vector, taken, arm] triple:
         - taken = 1: a world this timeline received; nothing contradicting it may be received here.
         - taken = 0: a world subtree this timeline foreclosed: it is the sibling minted where its parent took
           `vector`, so a message at or under `vector` is the parent's.
       `arm` (FlowCommitArm) says which mechanism minted the flow on the other side, which is what makes either
       refusal safe; only the producer knows it, at the push. Not a work item: it is this flow's identity as a
       receiver, carried by a fork and by the cold tier ('r' records). JS_UNDEFINED until a message arrives. */
    JSValue deliver_world_q;
    /* Cross-agent operations this instance was asked to perform: the record the asking instance wrote
       (core/frame/remote_op.h) and the trusted zone's rendezvous token for the waiting flow. Shaped like
       `deliver_q`, plus an answer: a document's state is its flows, so the record is attached to every live
       flow and each answers under its own delta. A FIFO, because the asking side parks on each operation in
       turn, so a second arriving before the first started is an ordinary second question.
       An entry is consumed when the flow turns it into a program: the token moves to that row (`dyn_token`) and
       the document is the row's `dyn_doc`. A JS Array of immutable [record, token] pairs, per flow because each
       arm answers its own copy. JS_UNDEFINED on a flow with nothing outstanding. */
    JSValue perform_q;

    /* Dev-only stamps for the assertion sub-linear ordering rests on: a member not holding the thread cannot
       change its own half of the weight while the frontier generation stands still. flow_weight is the family's
       coordinate plus this member's plus a carry bit; the family's half is a common offset within a family, and
       flow.c's flow_silence_phase decomposes the carry out, leaving flow_member_key.
       The walk stamps each member it weighs (it already holds the member and the key), and flow_age_running
       re-stamps the member it charges. Three operands are compared as one condition into one counter bucket,
       because key_checks_total() is asserted equal to the weighings one loop performed:
         - `key_last`: flow_member_key, the sum that must stand still or flow_pick goes blind to a fork
           re-ranking a whole arm;
         - `phase_last`: flow_silence_phase, `own_silence % FLOW_SERVICE_US`, which the key's floor discards and
           which is the bucket key; a sub-quantum writer would re-bucket a member without moving `key_last`;
         - `ikey_last`: flow_index_key, the key without the bucket term, which an index is keyed on; a fork's
           join raises `sub_born` for a whole arm with no generation bump, which the sum alone cannot see.
       `key_stamped` rather than a sentinel generation, because `g_gen` starts at zero and flow_registry_init
       resets it. Dev-only in field and check, since stamping is a call per member per scan. */
    /* Named residual. Not covered: `ikey_last` is a third evaluation because flow_weight re-associates its
       member half rather than containing it. Next diff builds: flow_weight composed with the member half as a
       subexpression, so one stamp covers both claims and this field is deleted. Absence shows as: a key check
       that passes while the index's key moved by an amount the bucket term compensated. */
#if APICLIENT_DEV
    double   key_last;
    unsigned key_gen;
    int      key_stamped;
    int64_t  phase_last;
    double   ikey_last;
#endif
} Flow;

/* `doc_name` is this instance's document identity, a parameter so a frontier cannot exist without one: every
   flow's world is named by it, and two instances sharing a name would hand each other's flows one segment. The
   host names the root document; every document below it is named by the one that created it. */
void  flow_registry_init(const char *doc_name);
void  flow_registry_free(JSContext *ctx);

/* Add a from-baseline flow to the frontier, with an empty decision vector. A flow with a recorded path has its
   `dec_blob` installed after the add (by the fork or the cold tier), because that path is a reference on a
   shared chain. Dups `fn`. Returns the stored Flow* (stable until removed). Never fails: OOM aborts via CHECK.
   flow_add seeds the flow with this agent's root document's programs as rows of its own queue
   (flow_set_seed_hook). flow_add_unseeded is for a creator whose flow must not start there: the fork, which
   inherits its parent's rows, and a joined document's boot flow, seeded with that document's programs. */
Flow *flow_add(JSContext *ctx, JSValueConst fn, WorldId parent);
Flow *flow_add_unseeded(JSContext *ctx, JSValueConst fn, WorldId parent);
/* Install the hook that gives a new flow its program sequence (solver/engine.c owns the document's script
   inventory). Installed when a session opens and removed when it closes, so this layer does not depend on what a
   document's programs are. */
void flow_set_seed_hook(void (*fn)(Flow *f));
/* Lifetime count of flows this document created; beside the switch count it says whether the frontier or the
   work per flow grew. */
long flow_created_count(void);

/* Is this flow blocked on the host, holding an unanswered synchronous request? The preempt hook always yields
   it and a mid-frame yield reports it host-owed, so the scheduler does not re-enter it to spin on an answer that
   cannot arrive while it holds the thread. */
int flow_blocked(const Flow *f);

/* Is this flow's JavaScript execution context stack empty? HTML §8.1.4.4 "Calling scripts", clean up after
   running script step 3: "If the JavaScript execution context stack is now empty, perform a microtask
   checkpoint." It guards both the checkpoint arm and the reply-delivery arm of the scheduler's ladder; the
   derivation is at the definition in flow.c. Exported for the census too: `framed` answers only the live-frame
   half, so the members that can take a task are not `flows - framed`. */
int flow_stack_empty(const Flow *f);

/* How many routed cross-document deliveries this flow holds: the length of `deliver_q`, with one reader of the
   queue's shape. 0 for an untouched flow's JS_UNDEFINED. */
int flow_deliver_pending(const Flow *f);

/* The three operations on `deliver_q`: append at arrival, take from the front at delivery, and a fork that gives
 * an arm its own Array naming the same entries (entries are never edited). Declared here because engine.c routes
 * and delivers while cold.c writes and reads it back, and one owner keeps the shape assert. Every mutation runs
 * inside cow_engine_write_begin/end: this is the scheduler's record about a flow, written from outside any
 * flow's delta (engine_route walks every flow), and a delta that captured it would restore a delivered message
 * when a sibling switched in. flow_deliver_take and flow_deliver_entry return an entry the caller owns. */
void    flow_deliver_push(JSContext *ctx, Flow *f, const char *record, const char *sender_origin);
JSValue flow_deliver_take(JSContext *ctx, Flow *f);
JSValue flow_deliver_entry(const Flow *f, int i);
JSValue flow_deliver_fork(JSContext *ctx, const Flow *parent);

/* The same operations over the commitment record `deliver_world_q`, a separate list because it outlives every
 * queue entry: the queue is what this timeline still has to do, this is what it has become. Each entry is
 * [vector, taken, arm]: the sending world's wire vector, whether this timeline received (1) or foreclosed (0)
 * it, and the FlowCommitArm below. Entries are never edited, so a fork shares them; the Array is per flow.
 * flow_world_commit_at returns an entry the caller owns. What a commitment means is engine.c's. */
/* Which mechanism minted the flow on the other side of the branch a commitment names. Both refusal kinds defer:
 * a received row leaves a contradicting record to the sibling arm minted where this timeline took that world,
 * and a foreclosed row leaves one at or under its subtree to the parent. Whether that flow exists is a fact
 * about the producing mechanism, stated at the push; a reader cannot re-derive it from the vector. A reader
 * that meets a value it has no arm for crashes.
 *
 * Named residual. Not covered: FLOW_COMMIT_ARM_ANSWER_OWED is an obligation flow_answer_fork discharges at the
 * peer's next answer, and a peer that answers once never discharges it, so the row cannot say whether its
 * refusal is held by an arm or lost. Next diff builds: the release criterion engine_host_take's residual names
 * (the peer stating every timeline holding a token has answered), splitting this member into the other two.
 * Absence shows as: the exhaustion assert over `_routedZeroDelivery` firing at session end after a timeline
 * refused a routed delivery on an answer commitment. */
typedef enum {
    /* No flow is on the other side; such a row's refusal is a message no timeline receives, so deliver_admits
       aborts on one. */
    FLOW_COMMIT_ARM_NONE = 0,
    /* The delivery-time fork (solver/engine.c's deliver_fork_arm) minted it, inheriting this flow's delivery
       queue at that instant, so it holds every record this flow consumes from there on. */
    FLOW_COMMIT_ARM_DELIVERY_FORK = 1,
    /* An arm is owed by another mechanism, later: flow_answer_fork mints it at the peer's next answer, which is
       not guaranteed. A reader may file it under neither neighbour: read as DELIVERY_FORK it asserts an arm that
       may not exist, read as NONE it aborts on a peer behaving correctly. Its refusals have their own census
       row (solver/step_unit.h). */
    FLOW_COMMIT_ARM_ANSWER_OWED = 2
} FlowCommitArm;

/* Is `a` a member of FlowCommitArm? The enum's only statement of its membership, kept beside it so adding a
   member is one edit. The values are solver/cold.c's park wire digits and their order means nothing, so never
   test a member with a comparison. */
static inline int flow_commit_arm_is_member(FlowCommitArm a) {
    return a == FLOW_COMMIT_ARM_NONE || a == FLOW_COMMIT_ARM_DELIVERY_FORK ||
           a == FLOW_COMMIT_ARM_ANSWER_OWED;
}

int     flow_world_commits(const Flow *f);
JSValue flow_world_commit_at(const Flow *f, int i);
/* Append a commitment. The macro captures the caller's __FILE__/__LINE__ and threads it to the coherence check
   inside, so a failure names the producer rather than one line of flow.c; it expands at each call site, so a
   new producer cannot omit its address. The spelling `flow_world_commit_push(` is kept because the abort and
   solver/cold.c's row census cite it. */
void    flow_world_commit_push_at(JSContext *ctx, Flow *f, const char *vector, int taken, FlowCommitArm arm,
                                  const char *file, int line);
#define flow_world_commit_push(ctx, f, vector, taken, arm) \
    flow_world_commit_push_at((ctx), (f), (vector), (taken), (arm), __FILE__, __LINE__)
JSValue flow_world_commit_fork(JSContext *ctx, const Flow *parent);

/* Lifetime count of rows appended to any flow's commitment record since the frontier came up; it cannot fall,
 * so two readings may be differenced. flow_world_commits is a gauge over current members, so this is what
 * tells a document where no timeline ever received from one whose receivers departed. Raised only at the
 * append (a fork shares rows and raises nothing). Reset in flow_registry_free beside the ask census it is read
 * against, so the two always cover one document. */
long    flow_world_commit_rows_written(void);

/* This flow's job queue and every operation on it, declared beside the field because engine.c enqueues, picks
 * and drops while cold.c counts, and one owner keeps the shape assert. A record is never edited after it is
 * pushed, so a fork shares the entries and hands each arm its own Array. A record holds no pointer:
 *   - the callee, as a session-local arrival ordinal into a table this file keeps, because a JSJobFunc is a
 *     static of the interpreter (promise_reaction_job, js_dynamic_import_job, host_call_job, …);
 *   - the arguments, as ordinary elements. They have no identity outside this session (a reaction's capability
 *     functions, an event init holding a WindowProxy), which is what keeps the queue un-parkable; a replay
 *     regenerates every job whose cause is inside the replayed program (see cold_park_flow for the exception);
 *   - the enqueuing realm's global object, the key of HTML §7.5.10 "Destroying documents" step 5 (a task whose
 *     document was destroyed is removed without running), held as a reference so a freed realm cannot dangle;
 *   - whether it is a task: the event loop performs a microtask checkpoint between tasks;
 *   - its JSTaskHandle, the runtime's name for it, held as a JS number (js_task_handle_new asserts < 2^53) so it
 *     matches an element's tracker slot exactly; JS_TASK_HANDLE_NONE names nothing;
 *   - its arrival stamp on the clock `dyn_run` shares (g_work_seq), as a checked JS number;
 *   - whether it came from outside the replayed program (flow_job_external_begin/end). */
/* Mutations run inside cow_engine_write_begin/end for pending.h's reason: this is the scheduler's record about a
 * flow, and a routed delivery or engine_unload_document's fan-out pushes onto a queue that is not the running
 * flow's, so a captured delta would restore a dropped job or remove a pushed one at a switch. */
int  flow_job_pending(const Flow *f);
/* Does it still hold a microtask? The checkpoint ends when it does not; a queued task is the next turn of the
   event loop, not part of this checkpoint. */
int  flow_job_microtask(const Flow *f);
/* How many of each kind. Both halves are returned, because deriving one by subtracting the other from
   flow_job_pending would make the partition identity unfalsifiable. See the census's `jobs_ready_task`. */
void flow_job_kinds(const Flow *f, int *task_out, int *micro_out);
/* Append, with `argv` dup'd into the record. `task` picks which of HTML §8.1.7 "Event loops"' two queues.
   `handle` is the runtime's name for the callback, so the host that took the job can find it again. */
void flow_job_push(JSContext *ctx, Flow *f, JSJobFunc *fn, int argc, JSValueConst *argv, int task,
                   JSTaskHandle handle);
/* The next arrival stamp: one monotone counter per instance, issued to both of a flow's task carriers (a `jobs`
   record at flow_job_push; a `dyn` row's `dyn_run`, and its `dyn_id` at creation). flow.c's g_work_seq says why
   the two share a clock: an order over arrival is the one §8.1.7.1 "Definitions" and §8.1.7.3 "Processing
   model" step 2.1 admit that no page can starve. Not a bound; only two stamps are ever compared. */
uint64_t flow_work_seq_next(void);
/* Does this flow hold a task older than the row at its cursor? §8.1.7.3 step 2.1's choice, answered from the
   stamps. `row_seq` is the cursor row's `dyn_run` (its arrival as a runnable item, never its name); the caller
   passes it because only the caller has established that a row exists (`seq_compiles`). Answers 0 when no task
   may begin: a DYN_POS_IMMEDIATE row at the cursor, an outstanding microtask, or an empty queue. */
int flow_task_precedes(const Flow *f, uint64_t row_seq);
/* The pick, which is HTML §8.1.7.3 "Processing model"'s microtask checkpoint and not a FIFO pop: the oldest
   microtask if there is one, else the oldest task. Removed from the queue and returned owned. */
JSValue flow_job_take(JSContext *ctx, Flow *f);
/* Run one entry: the callee called with its own arguments. The record's only reader, so callers hold an opaque
   entry. Returns the callee's result, owned. */
JSValue flow_job_run(JSContext *ctx, JSValueConst entry);
/* HTML §7.5.10 "Destroying documents", destroy a document step 5, for one flow: remove every job whose
   enqueuing realm is `realm`, without running it. Returns how many went. */
int  flow_job_drop_realm(JSContext *ctx, Flow *f, JSContext *realm);
/* Remove the job named `handle` from this flow's queue without running it; returns 0 or 1. This is quickjs.h's
   JSJobRemoveHook for a host that owns its jobs, used by HTML §4.11.4 "The dialog element"'s "Remove element's
   dialog toggle task tracker's task from its task queue". Finding nothing is ordinary: the task may have run, be
   running, or have gone with its document. Asked of one flow because after a fork each arm holds its own queued
   copy of one handle, and each arm's tracker names the copy in its own timeline. */
int  flow_job_remove(Flow *f, JSTaskHandle handle);
/* The arm's own queue: a new Array naming the parent's records, which are shared because none ever changes. */
JSValue flow_job_fork(JSContext *ctx, const Flow *parent);
/* How much of this flow's queue a replay would not re-cause. Read by the park, which may neither write it down
   nor silently drop it. */
int  flow_job_external(const Flow *f);
/* Bracket marking work enqueued from outside the replayed program (engine.c's flow_deliver). Never nested: a
   delivery is made from flow_step with no frame, which is asserted. */
void flow_job_external_begin(void);
void flow_job_external_end(void);

/* How many cross-agent operations this flow has been asked and not yet started: the length of `perform_q`,
   with one reader of its shape. 0 for an untouched flow, so the scheduler's pick stays a tag test. */
int flow_perform_pending(const Flow *f);

/* Does this flow still owe a peer an answer? Asked of the queue and the program rows together, because an
   operation is in one or the other from arrival until its completion is sent: a queued entry is unperformed,
   and a row holding a token is performed and unanswered. Every site that ends a flow reads it, because a token
   that dies with the flow leaves a flow in another instance suspended forever. */
int flow_owes_answer(const Flow *f);

/* The WFQ priority of a flow (higher = run sooner). Pure function of the flow's reward/aging/visit state. */
double flow_weight(const Flow *f);

/* The reward term as the order reads it: this flow's fork family's accumulated emitted value, not the member's
 * own `val`. Exported because the family node is private to flow.c and two consumers must name the quantity
 * flow_weight depends on: the scheduler's ranked-state cache, which asserts that the value yield fires only
 * when a weight term moved, and the cold tier, whose recipe carries (path, reward). Departed flows read 0.0. */
double flow_reward(const Flow *f);

/* The cold tier's rebuild only: replace the account a from-baseline flow was placed at with the reward the
 * parking session wrote down, so a resumed member returns at its own coordinate rather than the frontier's
 * virtual time. Anyone else wanting to raise a reward wants flow_credit_emit, a ledger entry. */
void flow_restore_reward(Flow *f, double val);

/* This flow completed a unit of work: the optimism term's visit. Credited by the scheduler after flow_step
 * returns, the one point that sees whole steps, when the flow is left between units (the same boundary as
 * HTML §8.1.7.3 "Processing model"'s end-of-checkpoint steps). Asserts the flow is not inside a program, since
 * thread time inside a program is what the term must not measure. */
void flow_credit_visit(Flow *f);

/* The scheduler handed this member the thread: the one writer of `picks`, credited at the context switch every
 * dispatch converges on. A member keeping the thread across consecutive steps is credited once, so
 * `picks == 0` means never chosen. */
void flow_credit_pick(Flow *f);

/* The census of what the ordering is made of. flow_weight is reward + optimism - aging, and the optimism term's
 * whole range is 1.0 (one emission), so a member a point below another in reward is outranked however long it
 * waits, until aging (FLOW_AGE_RATE per microsecond of silence) brings the top down. The reward spread therefore
 * decides whether the optimism term can still order anything; this struct measures it and the other terms.
 *
 * Read the populations precisely: `val_zero` (family reward exactly 0) is empty inside a busy period by
 * construction, because an unplaced from-baseline account's reward is the frontier's virtual time (flow.c's
 * FlowAcct `placed`) until it is first served. The members that earned none of their rank are
 * `members - self_emit`. Within one family that count describes the document's branching; with `families > 1`
 * and a reward spread it is one account outranking another whose members cannot act.
 * The weights themselves are reported (`w_top` and the gaps), since the order is their sum.
 *
 * Pure measurement: one scan, no reference taken, nothing mutated, safe between scheduler steps. It decides
 * nothing. A single sample characterises an instant, never a run. */
typedef struct {
    long members;      /* live members of the frontier — the denominator for every count below */
    /* The family reward's range over the frontier, read per member. A spread above 1.0 means the optimism bonus
       can no longer reorder its ends. Aging is subtracted only from the flow being served and its family, so it
       cannot lift a member that consumes nothing: the ends meet only by the top coming down, after
       `(val_top - val_min + 1) / FLOW_AGE_QUANTUM` quanta of the top's own plus family silence
       (flow_silence_notch), which every emission of that family resets.
       flow.c's flow_nonreward bounds every other term, so the reward gap is the only quantity that can put a
       never-run member behind the pick's choice; read it against `self_emit`. A one-family frontier has a
       spread of exactly zero, so a non-zero spread is a statement about several accounts (documents, @S
       searches, resumed recipes), read beside `families`. */
    double val_min;
    double val_max;
    double val_top;    /* …and flow_best's family's, so the top of the order is named rather than inferred */
    /* The frontier's virtual time at the census (flow.c's `g_vt`, SFQ's v(t)): the coordinate of the item in
       service and of every never-served account. Assigned unconditionally, so an empty frontier reports the
       clock. With `val_min` it says whether accounts are left behind (`val_min` far below `vt`) or the floor is
       where the queue stands; `vt` above `val_max` or frozen while `val_max` climbs means it stopped being a
       clock. */
    double vt;
    /* Members whose fork family's whole reward is zero: the population whose weight ceiling is 1.0. An arrived
       account has emitted nothing but holds the leader's coordinate, so it is outside this row (see
       `val_arrived`). */
    long val_zero;
    /* Members whose account has `earned == 0`: standing on the coordinate it arrived at, having produced nothing
       since. This is every from-baseline door (the @S candidate session, a joined document's boot flow, a
       cold-resumed recipe). With `val_unplaced` at zero, a large count at the reward floor is accounts served
       and out-earned (the bandit working); with `val_unplaced` large it is the placement relation failing.
       `self_emit` asks the same question of one member rather than its account. */
    long val_arrived;
    /* The subset of `val_arrived` never yet given the thread, whose coordinate is still a reading of the clock
       (flow.c's FlowAcct `placed`). An unplaced account's reward is `vt`, so `val_min` far below `vt` with a
       large count here is the order failing to reach members it holds at the clock. `val_arrived -
       val_unplaced` is accounts served that emitted nothing: the @S "dead candidate starves" population. */
    long val_unplaced;
    long self_emit;    /* members with val > 0: they emitted something themselves rather than standing on an
                          account an ancestor filled. Zero here while `finished` climbs is work that advances no
                          statement. */
    long unrun;        /* members at zero own silence (flow.c's flow_own_silence, never raw `Flow.cpu`): not
                          charged since their fork family last emitted, so it includes every arm of a family that
                          just produced (`never_picked` counts the never-dispatched). A superset of flow_pick's
                          `unrun`, which needs every non-reward term at zero; read against `vis_max` it separates
                          "nothing charged yet" from "nothing finished". */
    /* Members never handed the thread, and how far the best of them stands behind the front in the order's own
       points (the gap is 0.0 when the population is empty). `picks == 0` is the starved population: unlike
       `unrun`, `vis_zero` or `svc_min`, nothing the member did can move it.
       A gap of zero is not starvation: flow_pick's comparison is strict and the incumbent is the seed, so on a
       tied cohort (ordinary on a one-family page) the pick returns one of N tied maxima and the rest stand at the
       front with `picks == 0`. The tie is ended by strict demotion, not by a dispatch rule: flow_credit_visit
       lowers the optimism term at each completed unit, and flow_age_running charges the running flow's own
       silence by FLOW_AGE_QUANTUM steps. Relaxing the pick to `>=` would switch at every opcode.
       The finding is in the series: `never_picked` climbing across censuses while `members` grows is the tail
       not being reached (build.mjs reads the series). */
    long never_picked;
    double never_picked_gap;
    /* How many never-picked members stand exactly at the front (weight == `w_top`): the N of the tie above. A
       small N is a sweep in progress; a large N is an order whose within-family terms no longer separate
       anything, so `g_flows` position decides the pick. On a one-family frontier the reward and the family
       aging cancel, leaving the optimism bonus (raised only by flow_credit_visit, which asserts `frame == NULL`,
       and copied by a fork) and the own silence (forgiven for the whole account at an emission); read beside
       `vis_zero` and `jobs_framed` to see which left them tied.
       A gauge, never differenced. flow_wfq_census asserts `never_picked_at_top <= never_picked`, and that it is
       non-zero exactly when `never_picked_gap` is 0.0. */
    long never_picked_at_top;
    /* How the dispatches that did happen were distributed. With P = `members - never_picked` (live members ever
       chosen) and T = `picks_live`:
         T/P ≈ 1         the thread reached a fresh member nearly every time: the frontier grows faster than
                         one thread serves it, a throughput fact no weight term reaches;
         cohort swept    a served cohort re-picked ahead of a never-served tail, an ordering defect. Read it
                         off `flow_starved_picks_idle`, never off T/P: T/P also counts re-picks that continue
                         a framed member's program, which the strict comparison requires;
         picks_max ≈ T   one member holds the thread: a monopolizer the aging term is failing to sink.
       `picks_live` and `picks_max` are gauges over the members standing now (they fall when a member departs),
       never differenced. `picks_max` is biased against its own maximum, since the most-dispatched member is
       nearest to finishing, so compare it across runs only with `picks_departed` and `departures`.
       `picks_lifetime` is the lifetime counter (flow.c's `g_picks_total`), reset by nothing; it should equal the
       result's `_switches`, because flow_credit_pick's one caller sits beside engine.c's switch count.
       flow_wfq_census asserts `picks_live + picks_departed == picks_lifetime` and, separately,
       `picks_live <= picks_lifetime` (the equality alone admits a negative departed total). */
    int64_t picks_live;       /* GAUGE: dispatches held by the members standing now */
    int64_t picks_max;        /* GAUGE: the most any one of them holds */
    int64_t picks_lifetime;   /* LIFETIME: every dispatch this instance has made, departed members included */
    /* LIFETIME: the dispatches departed members took with them (flow.c's `g_picks_departed`, raised at
       flow_remove), so `picks_lifetime` is a published partition. `departures` partitions into
       `finished + sold + teardown` (engine_frontier_census asserts it). A finish costs a dispatch (flow_finish is
       reached only from the switched-in member's FLOW_STEP_DONE arm), so `departures > 0` with
       `picks_departed == 0` proves nothing finished; the converse does not hold, since a sold member may also
       have been dispatched. */
    int64_t picks_departed;
    /* The frontier's arrival and departure processes: lifetime counters, raised once per event, so they may be
       differenced. `arrivals / picks_lifetime`, members minted per dispatch, decides what a
       `never_picked_at_top` plateau means: below 1 the frontier drains and a plateau is the order failing to
       separate reachable members; above 1 no ordering drains it, and separating two untouched members reorders
       nothing anyone consumes. `members / picks_lifetime` is a gauge over a counter and is not a rate.
       It is a report and licenses no bound. flow_wfq_census asserts `arrivals - departures == members`; each has
       one writer (flow_new's append, flow_remove's swap-remove). `rank_changes` is not this count: most
       frontier_rank_changed callers are not membership changes (grep that call for today's list). */
    int64_t arrivals;
    int64_t departures;
    /* Findings offered to the order and how they split. Lifetime counters, never reset, so they may be
       differenced; flow_wfq_census asserts `credit_calls == credit_paid + credit_dropped`:
         `credit_calls == 0`                          no detector fired: a reach question, not an ordering one;
         `credit_dropped > 0`, `credit_paid == 0`     every finding was detected on host time, with no flow to
                                                      pay (expected for the root document's markup; flow.c's
                                                      `g_credit_calls` says why), so the reward is zero;
         `credit_paid > 0` with `val_top` at zero     impossible: flow_wfq_census's `f->val <=
                                                      acct_family_val(f)` fires on it.
       solver/endpoint.h's surface census partitions endpoint records and cannot answer this. A report, never a
       bound: no weight term reads them. */
    int64_t credit_calls;
    int64_t credit_paid;
    int64_t credit_dropped;
    int64_t svc_max;   /* the largest service notch in the frontier — who is actually consuming the thread */
    /* The floor of the own-silence notch. `svc_max` alone reads the same for one monopolizer on an idle frontier
       and for a frontier whose members all burned alike; `svc_max - svc_min` is the spread the ranking is made
       of. A from-baseline family is born at zero service (flow_arrive_at_virtual_time copies no silence), so it
       enters at the floor. The weight extrema cannot answer this: a uniform rise in service moves `w_top`,
       `w_min` and `cand_w_max` by the same amount. */
    int64_t svc_min;
    /* The optimism term's coordinate (completed units, `visits`), which no thread-time row can stand in for.
       `vis_max == 0` on a large frontier means no member has finished a program, so no queued job can have run
       (engine.c's job arms are all under `frame == NULL`); the pair's spread says whether turns rotate. */
    int64_t vis_min;
    int64_t vis_max;
    /* The family half of the aging in the same notches (flow.c's FlowAcct `fam_us`), which flow_weight reads.
       The reward lives on the same node, so a family presents it once however many arms it has. The ratio to
       `svc_max` is the widest family's fork factor, a fact about the document's branching;
       `svc_fam_max` far below `svc_max` cannot happen while every arm's charge lands on the family, which
       flow_age_running asserts. */
    int64_t svc_fam_max;
    /* The family half's floor. On a one-family frontier (a real page: every flow descends from boot) every
       member reads one `fam_us` (flow_fork_inherit joins the parent's account), so `svc_fam_max == svc_fam_min`
       is a common offset that orders nothing. A multi-family frontier with one chain monopolising shows the
       same maximum with a floor far below it. */
    int64_t svc_fam_min;
    /* How many families: the count of distinct family roots reached through the members (flow.c marks each
       root as the scan passes it), by identity, never by value. With the pair above: `families: 1` means the
       family half is structurally an offset; `families > 1` with equal extrema means level for now (a new
       family is born at zero on both halves and diverges at its first charge); `families > 1` with a floor far
       below the maximum means the family term is ordering. */
    long families;
    /* Members standing away from their family's epoch base, counted two ways. flow_own_silence is
       `cpu_gen == family->emit_gen ? cpu : 0`, and flow_credit_emit sends a whole family to its base by moving
       `emit_gen`, so an index over flow_index_key is rebuilt only for the members standing away.
       `epoch_away_live` is maintained incrementally at four sites, summed over distinct families;
       `epoch_away_walk` is this scan asking every member. flow_wfq_census asserts they are equal; both are
       published. Gauges: a sample lands anywhere between two emissions, so read one as how much of the
       frontier is off its base now. The cost question is `epochRebuildLifetime`, a lifetime counter. */
    long epoch_away_live;
    long epoch_away_walk;

    /* How many sub-quantum residues the frontier occupies: distinct values of flow_silence_phase. The aging notch
       `(own + fam) / S` decomposes exactly into the member's notch, the family's, and a carry bit whose
       threshold (the family's residue) is common, so between two frontier generations only that bit moves, and
       members sharing a phase flip it together. At 1 the bit is a common offset and one cached maximum is
       exact; that state is reachable (a fork copies `cpu` and `cpu_gen`, an emission zeroes a family's phases)
       but has been observed only with `members == 1`.
       The count is capped by the clock: phases are residues of `FLOW_SERVICE_US` in the quantum clock's
       currency, so a coarse clock bounds them at `FLOW_SERVICE_US` divided by its granularity. Read that
       ceiling off the run (the gcd of its microsecond accumulators) and never copy it from here: near it the
       figure is about the clock. A structure over the fixed phase domain `[0, FLOW_SERVICE_US)`, whose carry
       splits it into `[0, S-R)` and `[S-R, S)`, costs a logarithm whatever this reads, so prefer it.
       Read it against `picks_lifetime` first (only a charged member mints a residue) and `members` second; a
       quotient that falls monotonically as picks rise is saturation at the clock ceiling. */
    /* Named residual. Not covered: the residue ceiling is derived by the reader, not published. Next diff
       builds: the ceiling as a census row beside `sil_phases`. Absence shows as: a figure near the clock
       ceiling quoted as a statement about the frontier. */
    long sil_phases;
    /* How many members stand on the far side of the carry boundary now. A gauge (the threshold sweeps as the
       family burns and resets when its residue wraps), never differenced. 0 or `members` means the bit
       contributes nothing at this instant, which is an observation about one sample, not `sil_phases: 1`'s
       structural claim. */
    long sil_carry;

    /* The third accounting scope, a fork subtree, which says what the two sides of a branch received. Within a
     * family the member and family scopes cannot see a branch, and a real page is one family. A bucket is a
     * top-level arm: the node forked directly off a family root, reached from every member of its subtree in
     * one indirection (flow.c's FlowAcct `branch`). A deeper branch is summed into its top-level arm; the
     * residual at FlowAcct `up` says why there is no per-node bucket, and the `br_fan_*` rows name the deep
     * minter.
     * Kinds: `branches`, `br_live_*` and `br_depth_max` are gauges. `sub_born` and `sub_us` are per-bucket
     * lifetime counters, but an extremum over the buckets moves as buckets open and depart, so `br_born_*`,
     * `br_us_max/min`, the crowd and minter triples are read as ratios at one instant, never differenced. Only
     * `br_retired_us` and `charged_us` only grow. Burn is in microseconds of whatever `quantum_measure` answers
     * (thread CPU or wall, per the run's `@QUANTUM` `isCpu`), so quote a raw total with that line; a quotient of
     * two burns from one run is host-independent.
     * Identities asserted in flow_wfq_census, every term published: `br_live_sum == members` (buckets partition
     * the frontier) and `br_held_us + br_empty_us + br_retired_us == charged_us`, via `br_us_sum`. */
    /* Reading them: `br_live_max / members` is how concentrated the frontier is on one side of one branch;
     * `br_us_max / charged_us` is how concentrated the thread is. The extrema can belong to different buckets
     * (the burn maximum is often a departed family root holding nobody), so a one-arm pair is read off the
     * crowd or minter triple. The mint pair is the branch term's own range: flow_branch_bonus returns
     * `1.0 / sub_born`, so `1/br_born_min - 1/br_born_max` is the span wfq_accounted_spread reads. The live and
     * mint extrema fold only buckets holding a live member (a weight is read only for a standing member); the
     * `br_us_*` extrema do not, since receipt survives a departed subtree. `br_live_min` is 0 or 1 while a
     * family root's own bucket stands, since a root's bucket holds exactly that root.
     * Cost: a few adds per charge, fork, departure and node free, and per-member work inside the census's
     * existing walk; nothing per opcode and nothing that grows with fork depth. */
    /* Gauge: distinct top-level-arm buckets the walk took, empties included. branch_take raises it before its
       `live > 0` guard, and the family-root door takes a root's bucket after its flow departed to keep that
       burn in the identity, so it exceeds the live-bearing count by one per such family; it is not the fold
       width a per-bucket order would pay.
       Named residual. Not covered: the live-bearing bucket count is not published or derivable (`br_empty_us`
       misses a departed root that burned nothing). Next diff builds: that count, folded inside branch_take's
       live guard and emitted here, landing with `testing/live-wfq.js`, which throws on an unnamed branch-scope
       row. Absence shows as: `branches` quoted as a fold width, high by one per family whose root departed. */
    long branches;
    long br_live_max;   /* GAUGE: the most live members in one bucket — the fat side of a branch */
    long br_live_min;   /* GAUGE: the fewest; see above for why the root's bucket usually owns this */
    long br_live_sum;   /* GAUGE: their sum, published because `== members` is the partition identity */
    long br_born_max;   /* LIFETIME: the most members ever MINTED into one live bucket */
    long br_born_min;   /* LIFETIME: the fewest — the two ends of flow_branch_bonus's own denominator */
    /* The crowd: the fattest live bucket's own membership, mint and receipt, from one bucket. Its share of the
       members (`br_crowd_live / members`) against its share of the live buckets' thread
       (`br_crowd_us / br_held_us`) separates three states: at par, a branching arm converting fork factor into
       thread; near zero, the order demoting it while retention keeps the frontier from draining; above par, a
       monopolist. `br_crowd_us == br_us_max` says the crowd is the hungriest bucket; `br_crowd_born ==
       br_born_max` says it is the minter.
       flow_wfq_census asserts `br_crowd_live == br_live_max`, written by different writers (a dereference of the
       retained bucket after the walk; a running maximum during it). Folded inside the live guard: all three are
       zero only when no live bucket was reached, which cannot happen with members standing; a zero
       `br_crowd_us` with `br_crowd_live > 0` is a crowd never charged.
       Named residual. Not covered: what the hungriest bucket minted and shed. Next diff builds: nothing until a
       reading needs it. Absence shows as: `br_crowd_us`, `br_minter_us` and `br_us_max` all different with
       `br_us_max` inside `br_held_us`, a live arm holding the most thread that no triple names. */
    long br_crowd_live;    /* GAUGE: live members in the bucket that owns `br_live_max` */
    long br_crowd_born;    /* LIFETIME: that same bucket's own mint count */
    int64_t br_crowd_us;   /* LIFETIME MICROSECONDS: that same bucket's own receipt */
    /* The minter: the same three numbers for the bucket that has minted the most, a different arm whenever an arm
       shed what it minted (`sub_born = live + sub_gone`). A walk over an unknown length forks a `stop at n` arm
       at every position and each arm finishes, so the minter stands narrow and is not the crowd. It carries
       the smallest branch bonus, so `br_minter_us / br_held_us` against `br_minter_live / members` says whether
       the branch term's demotion reaches the thread (the crowd's three states, asked of this arm).
       `br_minter_gone` is the shed count, published nowhere else. flow_wfq_census asserts
       `br_minter_live + br_minter_gone == br_born_max` (different writers, as for the crowd);
       `br_minter_us == br_crowd_us` says the two selectors name one arm. Zero semantics are the crowd's. */
    long br_minter_live;   /* GAUGE: live members in the bucket that owns `br_born_max` */
    long br_minter_gone;   /* LIFETIME: arms that same bucket has SHED — published nowhere else */
    int64_t br_minter_us;  /* LIFETIME MICROSECONDS: that same bucket's own receipt */
    int64_t br_us_max;  /* LIFETIME MICROSECONDS: the most thread time one bucket's subtree ever received */
    int64_t br_us_min;  /* LIFETIME MICROSECONDS: the least */
    int64_t br_us_sum;  /* LIFETIME MICROSECONDS: their sum — one half of the burn identity */
    /* `br_us_sum` split by whether anyone stands in the bucket, so a live arm's share has a live denominator
       (`br_held_us`). Two accumulators in two arms of one condition, so `br_held_us + br_empty_us == br_us_sum`
       is a check; asserted in flow_wfq_census. These rows are gauges despite the per-bucket "lifetime" labels:
       a bucket whose subtree wholly departs is freed in `acct_unref` and its receipt moves into
       `br_retired_us`, so `br_us_sum`, `br_held_us` and `br_empty_us` fall then, and only `br_retired_us` and
       `charged_us` are monotone (solver/result.c files them the same way). */
    int64_t br_held_us;  /* LIFETIME MICROSECONDS: received by buckets holding at least one live member */
    int64_t br_empty_us; /* LIFETIME MICROSECONDS: received by buckets still taken and holding none */
    int64_t br_retired_us; /* LIFETIME MICROSECONDS received by buckets whose subtree has wholly departed */
    int64_t charged_us; /* LIFETIME MICROSECONDS the scheduler has charged at all — the identity's total */
    /* Gauge: how deep in the fork tree the deepest live member sits (0 at a root). It ranks nothing; it says
       whether the fork tree is a star (a root walk is O(1) amortised) or a chain (quadratic). Real bundles read
       a star, but the binding cost of a per-node bucket is retention, which depth cannot see (FlowAcct `up`). */
    int br_depth_max;

    /* Which fork inside a bucket did the minting, which the bucket rows cannot say because a deeper fork sums
     * into its top-level arm. A fan is the live members forked directly off one node, keyed on FlowAcct's
     * `up`, which for a live member is its true fork parent: `up` is written at flow_fork_inherit and moved only
     * by acct_compress_dead on a departing flow's own node. The parent may have departed and be retained by its
     * children, which is the shape these rows exist to name.
     * Over non-root parents only, gated by the predicate that decides the bucket (flow_fork_inherit's
     * `parent->acct->up`), since a fork off a root already has a bucket. Gauges, never differenced.
     * `br_fan_max / br_fan_sum` is how concentrated deep forking is; `br_fan_depth` locates the minter (1 when
     * a top-level arm minted the crowd itself); `br_fan_max == 0` says every fork is top-level. They cannot say
     * what a deep fork's sides received.
     * flow_wfq_census asserts `br_fan_max <= br_live_max` (a fan lies inside one bucket; the sides come from
     * different writers). No identity over `br_fan_sum` is asserted, because both terms would count trips of
     * the same loop. */
    long br_fan_max;    /* GAUGE: the most live members forked directly off ONE non-root node */
    long br_fan_sum;    /* GAUGE: live members forked off a non-root at all — the maximum's denominator */
    int br_fan_depth;   /* GAUGE: the fork-tree depth of the node holding that maximum; 0 when there is none */

    /* The @S candidate sessions, asked directly (`cand_src` set). `cand_unrun` counts candidates at zero own
     * silence, the order's quantity (flow_own_silence), so it also counts a candidate of a family that just
     * emitted. `cand_dec_max` is the furthest replay cursor any candidate stands at, in recorded decisions
     * (gates, not statements), taken from decide_blob_cursor for a parked candidate and decide_cursor for the
     * running one, because `dec_blob` is NULL while a flow runs. Read it against `dec_max`, the deepest decision
     * vector of any member: the share of the document's gate sequence the search has replayed. */
    long cand_members;    /* members carrying a payload substitution — a candidate session or a fork of one,
                             because engine.c copies the substitution to the sibling: a candidate that branches
                             is two candidates. So this is the search's whole live population, and @PROGRESS's
                             `candidates` (the searches seeded) is its root count. */
    long cand_unrun;      /* …of those, how many stand at zero own silence */
    int64_t cand_svc_max; /* …and the most service any one of them has consumed */
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
     *   `jobs_framed` — the member's execution context stack is NOT empty (`!flow_stack_empty`). The step
     *                   HTML §8.1.4.4 "Calling scripts" states as clean up after running script step 3 — "If
     *                   the JavaScript execution context stack is now empty, perform a microtask checkpoint" —
     *                   is what every job arm of flow_step is under, and flow_stack_empty is this engine's
     *                   statement of that sentence.
     *                   So these jobs wait on the member COMPLETING its unit of work, which is also what
     *                   advances the optimism term's `visits`.
     *                   AND `Flow::frame` IS NOT THAT STACK, WHICH THIS LINE SAID AND THE CENSUS OBEYED. A
     *                   live frame is one of the two things flow_stack_empty refuses; the other is a row at
     *                   the cursor marked DYN_POS_IMMEDIATE, which is the synchronous tail of the program that
     *                   queued it and is therefore stack the member is still standing on. Read `frame` alone,
     *                   the split handed those members to `jobs_ready` — see there for what a row naming the
     *                   wrong component costs, and solver/flow.c's job split for the repair.
     *                   This row is not a defect ON ITS OWN — it is the spec's precondition, measured — AND
     *                   THAT COVERS ONE OF ITS TWO READINGS, which is why the other is written here rather
     *                   than left to whoever meets it. A frontier some of whose members are part-way through a
     *                   program, and one in which not a single member is FINISHING one, produce the same row
     *                   and take opposite work. Framing is benign while frames END, and this is a gauge over
     *                   the members standing NOW, so a frame that ended leaves nobody standing to be counted
     *                   and this row cannot say whether they do. An unconditional clearance was therefore an
     *                   under-claim of the kind nobody discovers by acting on it, because acting on it means
     *                   not looking. THE DISCRIMINATOR IS ALREADY PUBLISHED AND IT IS NOT THIS ROW:
     *                   `stepUnitRuns`' `resume-ended-its-frame` against `resume-program` is the rate at which
     *                   a program that has survived at least one preempt ever completes, with `finished`
     *                   beside it — the same pair, and the same correction, that `deliv_framed` already
     *                   carries below and that this row was missing for longer. Read them before reading a
     *                   large `jobs_framed` as the precondition working.
     *                   AND THAT PAIR IS A FLOOR FOR BOTH OF THE QUESTIONS IT IS HANDED TO, BECAUSE IT IS ONE
     *                   RATIO ANSWERING TWO AND THE ROW WHERE THEY DIVERGE IS LEFT OUT OF EACH. The engine
     *                   states "a program COMPLETED" at the line that advances `g_completed`, and that line
     *                   stands ABOVE the naming line which splits the frame-clearing outcomes — so it covers
     *                   `resume-ended-its-frame` and `report-an-exception` alike. It states "this member is
     *                   UNFRAMED", which is what makes this row's whole ladder reachable, as
     *                   `f->frame == NULL` — which `resume-ended-its-frame` and `program-detached-its-base`
     *                   both satisfy and `report-an-exception` does not, the report taking the slot the
     *                   program just vacated. The two populations therefore differ by ONE ROW APIECE IN
     *                   OPPOSITE DIRECTIONS and `resume-ended-its-frame` is their INTERSECTION: read as a
     *                   completion rate it drops every program that ended by THROWING, and read as a
     *                   frame-clearing rate it drops every one that suspended at a TOP-LEVEL AWAIT. Forced
     *                   execution is what makes the first omission a POPULATION rather than an edge case — a
     *                   flow throwing on unknown input is the exploration surface working, so the row the
     *                   completion reading drops is one this engine produces on purpose. HOW LARGE IT IS ON
     *                   ANY RUN IS NOT ASSERTED HERE AND MUST NOT BE, because it is a number `stepUnitRuns`
     *                   already prints and a magnitude written into this paragraph would be a claim competing
     *                   with the command that answers it.
     *                   MEASURED, AND IT IS WHY THIS IS WRITTEN HERE RATHER THAN LEFT TO WHOEVER MEETS IT: a
     *                   lane drove two fresh browsers over one real application, read this prescribed pair at
     *                   2.3% and 1.1%, and reported that almost no program which survives a preempt ever
     *                   completes — a total taken from a floor, on the one ratio this file tells its reader
     *                   to take it from, and offered as the row that explained an empty learned surface. Sum
     *                   the arms for whichever question is being asked: they sum to `steps`, the addition is
     *                   free, and no single arm of this histogram is a rate.
     *                   RETIREMENT: this record goes when neither of the two readings above can be taken from
     *                   one arm of this histogram.
     *                   RETIRES when this row can no longer be read without that rate — that is, when a
     *                   frame-ending count stands on this census beside it and the pairing is one sample.
     *   `jobs_ready`  — neither: an empty stack and no mark, so the member reaches its jobs at the very next
     *                   pick it wins. These jobs wait on RANK ALONE, and they are the population §scheduler's
     *                   WFQ sentence is about.
     *                   AND THAT SENTENCE IS THE REASON THIS ROW HAS TO BE ASKED THROUGH THE ARM'S OWN GUARD
     *                   RATHER THAN A WEAKER PREDICATE: it names the component to open. A job counted here is
     *                   one the ORDER is holding, and a job the LADDER is holding reported here sends a reader
     *                   to flow_pick for a defect that is in flow_step — and it sends them with `job_w_gap`
     *                   beside it reading ~0, which says in as many words that the backlog is not an ordering
     *                   problem at all. Two rows, one wrong population, and the pair closes the question.
     *                   AND ITS ZERO IS TWO STATES, WHICH THIS SCAN'S OWN SHAPE DECIDES AND WHICH
     *                   `mem_unframed` BELOW IS WHAT SEPARATES. The arm is reached only inside `if (jn > 0)`,
     *                   so 0 is written both when NO member has `frame == NULL` at all and when unframed
     *                   members exist and hold no jobs. The first says the resume seam is not ending frames
     *                   and sends a reader to flow_step; the second says the jobs sit on members inside
     *                   programs while the members outside them hold nothing, and sends a reader to where
     *                   jobs are queued. Both are silences of the ORDER — the split above is right that
     *                   neither is the WFQ's to move — and they are not one finding. Read the two together:
     *                   `jobs_ready: 0` with `mem_unframed: 0` is the first, and with `mem_unframed > 0` the
     *                   second.
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
     * `jobs_ready_task` / `jobs_ready_micro` SPLIT THAT READY ROW AGAIN, ON THE AXIS THAT DECIDES WHICH ARM OF
     * flow_step CAN DISPATCH THE JOB. The three-way split above says what a job WAITS ON; this says which arm
     * TAKES it, and those are different questions. solver/engine.c's ladder puts the checkpoint arm
     * (`flow_checkpoint_due`, which is `flow_job_microtask && flow_stack_empty`) ABOVE the program sequence and
     * the task arm BELOW it, as the `else` of `if (seq_compiles)` — `a program of this flow's own sequence
     * STARTS on this step`. So for a member this scan has already admitted to the ready arm:
     *   a MICROTASK it holds makes `flow_checkpoint_due` true outright, because the ready arm's own guard is
     *   the second conjunct of it — so the SEQUENCE cannot exclude that job, the checkpoint arm standing above
     *   it;
     *   a TASK it holds is reached only on a step where the member holds NO microtask AND starts no program,
     *   so a member whose cursor names a runnable row takes the sequence arm instead, every time, for as long
     *   as the page keeps appending rows it can run.
     * `jobs_ready` calls both of those RANK-READY, and one of them is waiting on the order while the other is
     * waiting on the order AND on the sequence running out of rows.
     * WHAT THE PAIR ANSWERS IS A READING THAT HAS ALREADY BEEN MADE BY INFERENCE AND COST A LIFETIME STEP
     * HISTOGRAM TO MAKE. `jobsReady > 0` with a run's `_jobsRun` flat at zero is consistent with the checkpoint
     * declining and with the sequence declining, and those are two arms of one function taking opposite work.
     * AND THE CLAIM THE PAIR MAKES IS THE NARROW ONE, STATED AT THE STRENGTH IT WAS DERIVED AT: it is about
     * the SEQUENCE ARM'S EXCLUSION and about nothing else. All `jobsReadyTask` and no `jobsReadyMicro` is that
     * exclusion measured — every rank-ready job is behind the arm whose `else` binds to `seq_compiles`, so a
     * page that keeps appending runnable rows holds all of it, permanently. Any `jobsReadyMicro` at all
     * REFUTES that diagnosis for the jobs it counts, because `seq_compiles` stands below their arm and cannot
     * hold them — and it does NOT say they would have run, because flow_step has arms ABOVE the checkpoint too
     * (a peer answer, a declined request, a parked resume, a routed delivery, a cross-agent operation), each
     * of which is a unit of work in its own right rather than a program the page appended, and because the
     * PICK may not have reached the holder at all. Those are different work from an arm order, which is the
     * whole reason the row is worth taking before either fix downstream of it is priced.
     * THEY ARE GAUGES, by the same convention `jobs_ready` is one under and the same one the `Lifetime` suffix
     * elsewhere on this census marks the other kind with: one walk, the members standing NOW. Neither may be
     * differenced across samples, neither accumulates, and neither is a rate.
     * `jobs_ready_task + jobs_ready_micro == jobs_ready` IS THE IDENTITY, AND IT IS ASSERTED at the end of the
     * scan where all three are in one hand. The sides have DIFFERENT WRITERS, which is what makes it a check
     * rather than a restatement: the total accumulates the queue's own `length` and the halves accumulate a
     * walk of the queue's records, so it fires on an edit that moves one accumulation site and not the other.
     * engine/build.mjs asserts it again for `unframed_picks_lifetime`'s reason — the DCHECK is compiled out of
     * a release build that reader still runs over.
     * READ THEM BESIDE `jobs_ready` AND NOT INSTEAD OF IT: 0 and 0 is a frontier with no rank-ready job at all,
     * which is the pair of silences `mem_unframed` separates and neither of these halves can.
     * AND THE READING HAS BEEN TAKEN ON A REAL PAGE, WHICH IS WHAT THIS LEGEND WAS WRITTEN TO BE READ AGAINST.
     * Measured over one real application page, three drives, 87 censuses, through the installed artifact
     * stamped d18fa92658db25b9f64000ae7a16e10c9103f9da with a clean cone: `jobs_ready_micro` is ZERO in EVERY
     * census of all three, and `jobs_ready_task` equals `jobs_ready` in every one. Terminal rows 17/17/0,
     * 35/35/0 and 52/52/0 against 51, 98002 and 112156 framed and 4, 2803 and 3117 members, with
     * `jobs_owed` 0 throughout. By the arms above, that is THE SEQUENCE ARM'S EXCLUSION MEASURED: every
     * rank-ready job a real document holds stands behind the arm whose `else` bound to `seq_compiles` alone,
     * and none of it is behind the checkpoint. (That `else` binds to `seq_compiles && !job_precedes` now —
     * arrival order across the two carriers, argued at the ladder's own head — so the measurement above is a
     * reading of the state this pair was built to expose and not a description of the ladder today.)
     * The alternative arm this legend offers -- any `jobs_ready_micro`
     * at all, which would refute that diagnosis for the jobs it counts -- did not occur once.
     * THE ABSENT-VERSUS-ZERO CONTROL IS WHAT MAKES THE ZERO A READING RATHER THAN A SILENCE, and it was
     * armed by the same run: rows that artifact does not carry rendered as a DASH in the same output while
     * `jobs_ready_micro` rendered as `0`, so the driver distinguishes a row the build has no counter for
     * from a counter the run read as empty -- which is the one way this pair could have been misread.
     * WHAT IT DID NOT SETTLE WAS THE ARM'S POSITION, and the clause that stood here named the wrong ordered
     * predecessor — `the task source on a `jobs` entry rather than a reorder`. A source per queue is one
     * repair of two and the narrower one; what the ladder took is one ARRIVAL CLOCK across the carriers
     * (flow.c's g_work_seq), which needs the stamp on a `jobs` entry — that half was right — and does not
     * need the SOURCE there at all. The clause is kept because a reader who re-derives it from the sentence
     * above will re-propose moving a producer, which core/timing/task_source.h now argues against at its own
     * withdrawn absolute.
     * RETIREMENT: this record goes when a census row states which arm of flow_step declined a ready job at
     * the moment it declined it, because the pair is then a reading of a decision rather than an inference
     * from two gauges about a ladder. It is NOT retired by the position being settled: the pair still says
     * which arm holds the backlog, and a ladder with a settled order can still be wrong about it.
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
    long jobs_ready_task;
    long jobs_ready_micro;
    long vis_zero;

    /* HOW MANY MEMBERS HOLD NO FRAME — the denominator `jobs_ready` has always needed and never had, taken on
     * THIS walk beside `members` so the pair is ONE SAMPLE. It exists because a zero job count that has
     * correctly been declined to the ordering then has nowhere to go: the reader knows the backlog is not the
     * WFQ's to move and cannot say which of the two silences above it is looking at, and the count that would
     * have told them (`live - framed` off the COLD line) is a different walk at a different instant, so
     * pairing with it is a guess wearing two real numbers.
     * IT IS OVER MEMBERS AND `jobs_ready` IS OVER JOBS, deliberately, exactly as `vis_zero` stands beside
     * `jobs_framed`: the question is not how much backlog there is but whether there is anybody standing in
     * the state that could take it. The containment (`<= members`) and the implication (`jobs_ready > 0`
     * requires a member with no frame, because that arm is reached only through `!f->frame`) are asserted at
     * the scan, which is what keeps the two spellings of "unframed" from drifting apart at the two sites.
     * THE CONVERSE IS NOT ASSERTED AND MUST NOT BE: `mem_unframed > 0` with `jobs_ready == 0` is the SECOND
     * silence, which is the whole reason this row is here. */
    long mem_unframed;

    /* …AND HOW MANY DISPATCHES THAT POPULATION HAS EVER RECEIVED — the LIFETIME half of the row above, and
     * the one row on this census that can say whether the ORDER has ever offered the thread to a member
     * standing in the state the row above counts. It is raised in flow_credit_pick, beside `picks_lifetime`
     * and conditionally on the same flow_stack_empty the job and delivery splits are asked through, so the
     * numerator and the population it is drawn from move together and the containment is by construction.
     *
     * THE KIND IS IN THE NAME AND IT IS THE OPPOSITE OF ITS NEIGHBOUR'S. `mem_unframed` is a GAUGE: a member
     * that departs or that frames itself by running leaves it, so it may FALL between two censuses and
     * differencing it is arithmetic over no quantity. This is a LIFETIME COUNTER, raised once per event and
     * lowered by nothing, so it is the one of the pair a reader may difference across two samples — and a
     * series of it that DECREASES is the free tell that it has stopped being one.
     *
     * READ IT BESIDE `picks_lifetime` AND NEVER ALONE, because a zero has an absent reading and a measured one
     * and they take opposite work. With `picks_lifetime` at 0 this instance has dispatched nothing at all and
     * the row is silent about the order; with `picks_lifetime` large it is a measurement. That pair is the
     * denominator rule CLAUDE.md states for every count offered as a share of another, and it is the whole of
     * what makes the two readings below distinguishable:
     *
     *   0 with dispatches made   the order has NEVER handed the thread to a member with an empty stack, and
     *                            this arm is DECISIVE FOR THE READY HOLDERS TOO. `jobs_ready`'s arm is
     *                            reached only through flow_stack_empty — the assert at the end of this scan
     *                            states exactly that — so a ready holder is inside this row's population by
     *                            construction, and a zero here is a zero for it. Every reading this census
     *                            publishes about that population (`jobs_ready`'s "waits on RANK ALONE",
     *                            `job_w_gap`'s "the front of the order is holding one") is then a statement
     *                            about members the dispatch does not take, and the defect is in the DISPATCH
     *                            PATH rather than in the terms: the front of the order is one of these
     *                            members and flow_next_to_run's caller is not running it.
     *   above 0                  "ranked at the front and never taken" is REFUTED for the unframed population
     *                            as a whole, and THAT IS ALL IT ESTABLISHES — stated as a refutation rather
     *                            than as a proof because the two are not the same claim and this row can only
     *                            make the first. What is left open is whether `w_top` and the ready holder's
     *                            weight are the quantities the dispatch compares at all, which is where a
     *                            gap of zero over members the order really does reach has to be read next.
     * IT HAS BEEN READ AND THE SECOND ARM IS THE ONE TAKEN, recorded here because the residual in solver/flow.c
     * that asked for this row says whichever reading a run establishes is recorded AT THE SITE THIS ROW NAMES
     * rather than there. Measured on two real documents through one artifact stamped 18550a41 with a clean
     * cone, 146 censuses over three runs: this reads 4 and 5 against `picks_lifetime` 9 and 10 on one site and
     * 280 against 619 on the other, with `_jobsRun` 0, 0 and 8. "Ranked at the front and never taken" is
     * REFUTED. The order DOES hand the thread to these members, so the DISPATCH PATH is not the defect and the
     * reader goes to flow_step's ladder rather than to flow_pick.
     * AND THE ASYMMETRY THAT USED TO BE THE PRICE OF THE PAIRING IS COUNTED NOW RATHER THAN ARGUED — the
     * residual that stood here asked for `ready_picks_lifetime` and it is published below, so this arm is a
     * BOUND on the ready holders and the row beside it is the measurement. What is deleted with the residual
     * is a LICENCE and not a fact: it said the uncovered population was empty wherever
     * `jobs_ready == (jobs / members) * mem_unframed` held, because a fork byte-copies its parent's queue and
     * nothing had consumed one. That identity is a property of a frontier THAT HAS RUN NO JOB, which is the
     * state a reader holding a flat job count is investigating — so the licence was available exactly where it
     * could not be checked, and flow.c's `g_ready_picks_total` carries both arms of it measured.
     *
     * WHAT WOULD MAKE IT UNTRUSTWORTHY, stated here because a row whose failure modes are not written down is
     * one a reader will rationalise after the fact. Three things, and each already has a check: the
     * containment against `picks_lifetime` (asserted at the end of flow_wfq_census, and re-asserted by the
     * build's reader because the DCHECK is compiled out of a release); `picks_lifetime == _switches` across
     * the document, which is what says this census's denominator is the engine's own dispatch count and not a
     * second one; and a re-spelling of flow_stack_empty at the RAISE but not at the census's arms, which
     * cannot happen while both call the function rather than restating it. If any of the three is broken the
     * row is a count of some other event and neither reading above is available.
     *
     * IT IS A REPORT AND NOT A BOUND. Nothing reads it inside the ordering, no fork carries it, nothing resets
     * it and nothing branches on it — which matters more here than for the pick rows beside it, because a
     * count of dispatches a population has not received is exactly the numerator a watchdog over a flow that
     * never finishes anything would be built from. §NO BOUNDS forbids that, and flow_credit_pick's own
     * paragraph states the property this leans on: a quantity the ordering consumes stops being able to
     * answer the question it exists for. */
    int64_t unframed_picks_lifetime;

    /* …AND HOW MANY OF THOSE DISPATCHES REACHED A MEMBER THE READY ARM WOULD HAVE COUNTED — the row that turns
     * the counter above from a BOUND on the job backlog into a MEASUREMENT of it, and the one this census was
     * missing when a run reporting `unframedPicksLifetime` in the tens beside `jobsRun: 0` was read as the
     * dispatch reaching the backlog and finding nothing to run.
     * THE PREDICATE IS THE READY ARM'S OWN THREE CONJUNCTS and not a fourth spelling of them: flow_credit_pick
     * raises it under `!flow_host_owed && flow_stack_empty && flow_job_pending > 0`, which is the `if / else if
     * / else` above restated as one condition, so a member counted here is one the job split would have put in
     * `jobs_ready` at that instant.
     * IT IS A LIFETIME COUNTER AND `jobs_ready` IS A GAUGE, which is the whole reason it is a second row: a
     * ready holder that is dispatched and then FRAMES ITSELF by running leaves the gauge and stays in this, so
     * the gauge cannot say whether the order has ever offered one of them the thread. Differencing this across
     * two samples is arithmetic over dispatches; differencing `jobs_ready` is arithmetic over nothing.
     * READ IT BESIDE `unframedPicksLifetime` AND `jobsRun`, WHICH IS THE ONE READING IT EXISTS FOR AND IT IS
     * THREE-WAY:
     *   0 with `unframedPicksLifetime` > 0   the dispatch reaches unframed members and NEVER one holding a job.
     *                                        `jobsRun: 0` is then about WHO IS PICKED, and the reader goes to
     *                                        flow_pick and to what the ready holders' weight is.
     *   > 0 with `jobsRun` 0                 the dispatch DOES reach job holders and flow_step declines the job
     *                                        at an arm above the one that would run it. The reader goes to the
     *                                        LADDER, and `jobsReadyTask`/`jobsReadyMicro` say which arm.
     *   0 with `picksLifetime` 0             the instance has dispatched nothing and this row is silent.
     * The first two are the third state solver/flow.c's job-split residual had to add to its own dichotomy, and
     * this is the row that decides between them instead of a reader inferring it from `jobWGap`.
     * AND THE ZERO ARM'S CONCLUSION IS UNSOUND, WHICH IS A CORRECTION TO THIS LEGEND AND NOT TO THE ROW — IT
     * NAMES THE WRONG COMPONENT, AND IT NAMES THE ONE THE PARAGRAPHS AROUND IT SPENT A SESSION MOVING A
     * READER AWAY FROM. flow_credit_pick has exactly one caller and it is engine.c's `best != cur` block, so
     * this counts DISPLACEMENTS and not dispatches: engine_sched_step calls flow_step on `cur` OUTSIDE that
     * block, and flow_next_to_run DEFENDS THE INCUMBENT ON A TIE — flow_pick folds the seed back on `>=`, so
     * the maximum is over {seed} union members with ties to the seed and then to registry order. A ready
     * holder RETAINED as incumbent is therefore STEPPED for as long as it holds the top and is credited here
     * NOT ONCE. A zero is consistent with "no ready holder was ever offered the thread" AND with "a ready
     * holder held the thread continuously and the ladder declined its job at every step" — the ORDER reading
     * and the LADDER reading, which is the one pair this row exists to separate.
     * THE TRAP IS ALREADY WRITTEN DOWN FOR THE SUPERSET AND THIS ROW INHERITS IT BY CONSTRUCTION, being
     * raised inside `unframed_picks_lifetime`'s own `if`: solver/engine.h's `unframed_steps` states that
     * pairing the two by name IS the trap and records it measured on one live page at 3 against 75. A subset
     * of a row that undercounts the descents by that factor cannot carry a reading whose whole content is a
     * zero, and on a frontier standing at one weight — which is the state this engine's own measurements
     * report for a real page — the retained incumbent is the dominant case rather than a corner.
     * WHAT ANSWERS IT WITHOUT THE HOLE IS ALREADY PUBLISHED AND IS NOT ON THIS CENSUS. `unframedStepsLifetime`
     * is raised at the line that ENTERS flow_step's `if (!f->frame)` ladder, on every pass rather than on
     * every switch, so it is nonzero exactly when the ladder was descended; `stepUnitRuns` beside it says
     * which arm took every descent, and `run-a-task` is the arm a ready job would have left in. Read that
     * pair FIRST. This row is then a statement about displacements and is worth what a displacement count is
     * worth.
     * WHAT IS NOT WITHDRAWN IS THE `> 0` ARM, which is a REFUTATION and needs no completeness: a positive
     * count is a dispatch that really did reach a ready holder, however many uncredited steps went with it.
     * Only the arm whose whole content is an ABSENCE is affected — which is the under-claim asymmetry
     * CLAUDE.md names, arriving inside a legend rather than inside a finding, and it is the direction nobody
     * discovers by acting on it, because acting on it means going to flow_pick and finding nothing there.
     * RETIREMENT: this correction goes when the count is raised where the STEP is entered rather than where
     * the switch is credited, because a retained incumbent is then inside the population and the zero means
     * what the arm above says it means.
     * CONTAINED IN THE ROW ABOVE BY CONSTRUCTION — raised inside its `if` — and asserted at the end of
     * flow_wfq_census, which is the arithmetic tell CLAUDE.md names for every count offered as a share of
     * another: a subset exceeding the population it claims to be drawn from.
     * THE CONTAINMENT IS DEV-ONLY UNTIL A READER OF THE DOCUMENT ASSERTS IT, WHICH IS AN ACT AND NOT A WAIT:
     * the DCHECK is compiled out of the release build every real-page drive uses, so the check that makes
     * this a counter rather than a digit is absent exactly where the row will be read. engine/build.mjs
     * already re-asserts `unframedPicksLifetime <= picksLifetime` for that reason and is where the same
     * line for this pair belongs; whoever owns that reader adds it. Until then a release census carries
     * the pair unchecked, and a reader who meets them out of order is meeting an unasserted ratio.
     * IT IS A REPORT AND NOT A BOUND, for `unframed_picks_lifetime`'s reason and under the same ban. */
    int64_t ready_picks_lifetime;

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
     *                    should be zero, because the assert at the mark admits one only for an entry the HOST
     *                    CAN STILL BE ASKED ABOUT (`pending_host_outstanding`) or a referenced document — so a
     *                    non-zero row here beside `pendReady == pend` is those two statements disagreeing.
     *                    THE TWO PREDICATES ARE NOT THE SAME ONE AND THIS ROW IS WHERE THAT SHOWS. A DECLINED
     *                    entry is OUTSTANDING and is owed by nobody, so a frontier holding one has a member the
     *                    selecting arm marks and the assert refuses; the row is what a reader sees if that
     *                    assert is compiled out. In release it is the shape to look for behind a document that
     *                    stops getting deeper while `live` and `blocked` both look healthy.
     *   `deliv_framed` — the member fails flow_stack_empty, so the reply-delivery arm cannot run for it. This
     *                    is HTML §8.1.4.4 "Calling scripts"'s clean up after running script step 3 measured,
     *                    not a defect on its own — exactly as `jobs_framed` is not.
     *                    AND THAT SENTENCE COVERS ONE OF THIS ROW'S TWO READINGS, WHICH IS WHY THE OTHER IS
     *                    WRITTEN HERE RATHER THAN LEFT TO WHOEVER MEETS IT. A frontier some of whose members
     *                    are part-way through a program, and one in which not a single member is FINISHING
     *                    one, produce the same row and take opposite work — and the sentence above names only
     *                    the first, so it reads as a clearance for both. Framing is benign while frames END;
     *                    this row cannot say whether they do, because it is a gauge over the members standing
     *                    NOW and a frame that ended leaves nobody standing anywhere to be counted.
     *                    THE DISCRIMINATOR IS ALREADY PUBLISHED AND IT IS NOT THIS ROW, so nothing here needs
     *                    a counter. `stepUnitRuns` counts the ladder's arms over the instance's life, and
     *                    `resume-ended-its-frame` against `resume-program` is the rate at which a program
     *                    that has survived at least one preempt ever COMPLETES. It is the only one of the two
     *                    frame-ending rows that can free a reply-holder: `start-ended-its-frame` is a row
     *                    that ended in the step that STARTED it, which a program long enough to issue a
     *                    request and go on running never is. `finished` beside them is how many flows have
     *                    ever retired.
     *                    AND `ever COMPLETES` IS THE HALF OF THAT SENTENCE WHICH TRAVELS, WHICH IS WHY THE
     *                    CORRECTION LIVES AT `jobs_framed` AND IS POINTED AT FROM HERE. The clause after it
     *                    is EXACT for this row's own question — freeing a reply-holder needs
     *                    `f->frame == NULL`, and `report-an-exception` leaves the report standing in that
     *                    slot — while the completion reading in front of it drops that same row, because
     *                    `g_completed` is advanced above the line that names either. A reader who carries the
     *                    sentence away carries the WIDER claim, and one has. See `jobs_framed` for the two
     *                    populations, for the row each reading omits, and for what quoting one arm as a rate
     *                    has already cost.
     *                    WHAT A FRAMED ROW NEAR `live` MEANS WHEN THAT RATE IS NEAR ZERO is not that the arm
     *                    lost a ranking. flow_stack_empty's first line is `if (f->frame) return 0;` AND
     *                    engine.c encloses its whole task ladder — the delivery arm with it — in
     *                    `if (!f->frame)`, so such a member's steps never reach the arm's line at all. That
     *                    makes `deliv_ready` the WHOLE population any ordering could serve, and a zero
     *                    delivery count read as starvation is a fraction of a population of that size.
     *                    MEASURED, three runs at three revisions of one day (a08a1158, ca96fc52, e08db848),
     *                    each the last @COLD of its own smoke: framed/live 552/561, 309/322 and 635/635,
     *                    with `resume-ended-its-frame` 9, 6 and 2 against `resume-program` 690, 182 and 1519,
     *                    `finished` 0 in all three, `pendReady == pend` at 46456, 48092 and 45115
     *                    answered-and-untaken entries, and `deliver-one-reply` 0, 0 and 1. The arms sum to
     *                    `steps` in each run, so the split needs nothing but addition: 699 of 874, 188 of 283
     *                    and 1521 of 1548 steps never entered the block. Add them up against `steps` before
     *                    reading any one arm as a rate, and quote the revision beside whichever you quote.
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
    /* WHICH TERM `deliv_w_gap` IS MADE OF, AT THE TWO MEMBERS IT IS BETWEEN — the one reading that row cannot
     * be given without them, and the reason it is the OPTIMISM term's operand rather than a decomposition of
     * the whole weight. Every other summand of flow_weight already has a row a reader can price the gap
     * against: the reward is `val_top` and moves in whole emissions, the aging is `top_svc`/`top_svc_fam` and
     * moves at FLOW_AGE_QUANTUM per quantum, the fitness distance is `dist_max` and steps by 1/FLOW_RUNGS_N.
     * The optimism bonus is 1/(1+`visits`), and its step SHRINKS with the count — so the same numeric gap is a
     * different number of turns depending on where on the curve the two members stand, and `vis_min`/`vis_max`
     * are extrema over the WHOLE frontier and are silent about these two. A reader holding only those rows can
     * compute which term a gap COULD be and cannot say which it IS.
     * A GAUGE, NEVER DIFFERENCED, like every row it sits beside: both members can be replaced between two
     * samples, so the series falls as well as rises and a difference of two of them is arithmetic over no
     * quantity. Read them as a PAIR and only where `deliv_ready` is non-zero — with no ready holder there is no
     * gap to state and both are 0, exactly as `deliv_w_gap` is.
     * WHY THIS PAIR AND NOT A PER-MEMBER DUMP: the question a reader brings to `deliv_w_gap` is whether the
     * members holding an undelivered reply are behind the front by something the ORDER decided or by something
     * they EARNED, and `visits` is the only term of the four that a member can only raise by FINISHING a turn
     * (flow_credit_visit asserts `frame == NULL` and no owed checkpoint). A fork inherits its parent's count
     * (flow_fork_inherit), so an arm that has never completed anything reads its parent's, and a member that
     * completes one drops below every arm it forked by exactly one step of this curve. Whether that is what a
     * given gap IS, is what these two rows say and nothing else here can.
     * THE TYPE IS `Flow::visits`'S AND NOT A NARROWER ONE, for `vis_min`/`vis_max`'s reason exactly: this host
     * compiles for wasm32, where `long` is 32 bits and the field is 64, so a `long` row here would be a silent
     * narrowing of the one quantity the pair exists to state. */
    int64_t deliv_w_gap_vis;  /* GAUGE: `visits` of the best READY holder — the member `deliv_w_gap` is from */
    int64_t w_top_vis;        /* GAUGE: `visits` of the member at `w_top` — the member `deliv_w_gap` is to */

    /* HOW FAR THROUGH THE DOCUMENT'S OWN PROGRAM TABLE THE FRONTIER'S DEEPEST MEMBER HAS GOT, AND WHAT THE
     * ORDER OFFERS IT — the one sentence neither this census nor the cursor histogram can say alone, and the
     * one that separates the two opposite diagnoses a piled-up frontier has.
     *
     * WHAT IS MISSING WITHOUT IT. `programCursors` (solver/cold.h) says WHERE the members stand and every row
     * of this struct says WHAT THE ORDER IS OFFERING, and nothing joins them: a frontier reading `{7: 71296,
     * 8: 156}` is either an order that ranks the 156 who got through program 7 AT THE FRONT — in which case
     * the members are being offered the thread in the right sequence and the tail is not being reached for
     * want of dispatches — or an order that ranks 71296 members still at row 7 AHEAD of them, in which case
     * the sequence itself is wrong. THOSE TAKE OPPOSITE WORK: the first is repaired by finding where a TURN
     * GOES and the second by a TERM, and flow.c's block at `g_arrivals` records the same run being dispatched
     * as the second when the rows it quoted could only have shown the first.
     *
     * IT IS THE `deliv_w_gap` SHAPE AND NOT A NEW ONE — a difference against `w_top`, with the population it
     * is a maximum over published beside it, written in ONE branch so a reader who finds a gap is holding the
     * count it is about. `cur_deep_w_gap` is 0.0 exactly when a member standing at the deepest row is itself
     * at the front of the order, which is the order having nothing to answer for; a positive gap is the
     * distance the front stands ahead of every member that has run furthest, in the same points
     * `never_picked_gap` and `nonreward_max` are in, so a reader can price it against one emission's worth
     * without a second rule.
     *
     * IT COSTS NO WEIGHING. The walk has already computed each member's weight for `w_min`/`w_top`, so these
     * three are collected off a number that was going to be taken anyway — which is the bar flow.h's
     * FLOW_SCANS sets for anything this census does, and `scanCensusWeights` is unchanged by them.
     *
     * THE KINDS. `cur_deep` and `cur_deep_live` are GAUGES over the members standing NOW: both may FALL
     * between two samples (a member at the deepest row departs, or one advances past it and takes the whole
     * population with it), so neither may be differenced and neither is a high-water mark — `deepest` and
     * `deepest_left` (solver/engine.h) are the monotone pair and these are deliberately not them. The gap is a
     * reading at an instant like every other weight row here.
     *
     * READ AGAINST `programCursors` ON THE SAME SAMPLE AND THE PAIR CHECKS ITSELF. `cur_deep` is the top
     * non-empty index of that histogram and `cur_deep_live` is that bucket's count, computed by a DIFFERENT
     * WALK in a different file over the same `Flow.script_i`; when the two censuses carry one `workDone` they
     * must agree, and a disagreement is two walks reading two frontiers. That is a reading and not an assert,
     * because the two are composed by two functions and nothing in this engine guarantees they were taken at
     * one instant — which is precisely why the top bucket is REPEATED here rather than left to be joined
     * across two objects, the same correction `workDone` on this line already made for `_unitsDone`.
     *
     * A REPORT AND NEVER A BOUND (§NO BOUNDS). No term of flow_weight reads any of the three, no pick branches
     * on them, nothing is shed or capped by them; a cursor entering the order would be a term monotone in a
     * quantity a fork carries FORWARD, which ranks the youngest arm highest — the LIFO CLAUDE.md names, not a
     * drain order — and this row exists to say whether such a term is even called for before anybody writes
     * one. */
    int  cur_deep;        /* GAUGE: the deepest `Flow.script_i` any live member stands at; 0 on an empty walk */
    long cur_deep_live;   /* GAUGE: how many live members stand there — the gap's own population */
    double cur_deep_w_gap; /* `w_top` minus the best weight offered by a member standing at `cur_deep` */
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
 *   `rival-of-incumbent` is the PREEMPT HOOK's, and its cadence is set by TWO things rather than one — the
 *      key it caches on is `flow_frontier_gen() != g_seen_gen || cur != g_seen_cur`. Every fork, arrival,
 *      departure, emission, completed unit and host-owed transition calls frontier_rank_changed, so the
 *      cached rival goes stale and the next opcode rescans; and an INCUMBENT SWITCH invalidates it just as
 *      readily, because the rival is `best eligible OTHER than cur` and only the excluded member has to move.
 *      So a forking page pays this per fork AND a dispatching one pays it per switch, and a quotient taken
 *      over forks alone has been read as a second raise per fork when it is the other disjunct — see
 *      solver/flow.c's measurement paragraph for that retraction, and `rivalMissGen`/`rivalMissCur`/
 *      `rivalMissBoth` (solver/engine.h) for the partition that tells the two apart. A raise is NOT a miss:
 *      raises with no interpreter opcode between them collapse into one.
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
    /* the preempt hook's rival rescan — one per MISS of a key that is a DISJUNCTION:          \
       the frontier generation OR the incumbent. Not "one per generation change": the rival is  \
       `best eligible OTHER than cur`, so a switch invalidates the cache with the frontier       \
       standing still, and raises made inside one C call collapse into ONE miss at the next      \
       poll. Which half a miss came from is `rivalMissGen`/`rivalMissCur`/`rivalMissBoth`        \
       (solver/engine.h), a partition of this row asserted at the census. */                     \
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
int64_t flow_scan_weights(FlowScan s);   /* per member per scan — 64-bit for FlowKeyChecks' reason */

/* …AND WHAT THE ONE ASSERTION IN THAT SAME WALK DID WITH THE WEIGHTS THOSE ROWS COUNT — the partition the
   banner above is the exact complement of. FLOW_SCANS counts the flow_weight the scan PERFORMED "and never
   the ones a DCHECK below it makes"; this counts what that DCHECK did, and the two are disjoint by
   construction.
   WHY IT IS A ROW AT ALL: flow_pick's member-key invariant is a predicted ABSENCE — the claim is that it
   never fires — and a clean run is satisfied identically by an invariant that HOLDS and by a path NOBODY
   TOOK. Its condition is a four-way disjunction whose first three arms EXEMPT the member (it holds the
   thread, it has never been weighed, or the generation has moved since it was), so a scan can walk a whole
   frontier and compare nothing whatever. `armed` is the number of comparisons actually made and is the only
   quantity that scores that prediction. Until this row the arming could only be INFERRED, by pigeonhole,
   from four scan counters against `rankChanges`.
   THE OTHER THREE ARE NOT DECORATION AND THEY TAKE DIFFERENT WORK. `running` is bounded by one member per
   scan and `first_seen` by one per member ever created, so both are structurally small; `stale_gen`
   approaching the total is the FRONTIER GENERATION MOVING FASTER THAN MEMBERS ARE RE-WEIGHED, which makes the
   invariant vacuous rather than held — a finding about the frontier, and the one a bare `armed: 0` could not
   distinguish from a quiet engine.
   ONE STRUCT AND ONE CALL, because the four are a PARTITION and a partition read through four calls is four
   moments. §Testing's rule is that a conservation identity holds WITHIN ONE SAMPLE and nowhere else, so this
   is taken at one instant by construction rather than by the caller remembering to.
   THE KIND IS THE SAME FOR ALL FOUR AND IT IS IN EVERY PUBLISHED NAME: LIFETIME COUNTS OF COMPARISONS, never
   reset and monotone, which is the only kind a reader may DIFFERENCE. None of them is a gauge over the
   frontier and none is per-member, so none may be read against `members` as a share of anything.
   ZERO IN A RELEASE BUILD, AND THAT IS NOT A READING OF ANYTHING. The check and its per-member stamp are
   `#if APICLIENT_DEV` (see `Flow.key_last` for why), so nothing raises these where the product ships — while
   solver/result.c publishes unconditionally, because that composer has no dev arm at all and engine/build.mjs
   takes its REQUIRED row set from that composer's own format string, so a row emitted in one build only would
   fail every release census this repository takes. The discriminator needs no sentinel and is already on the
   line: ALL FOUR AT ZERO WITH ANY `scan<Entry>Weights` NONZERO IS A BUILD THAT MAKES NO CHECK, because in a
   dev build every member the dispatch loop weighs raises exactly one of the four. `armed: 0` with the four
   summing ABOVE zero is the real finding — the walk ran, and compared nothing.
   THE IDENTITY IS ASSERTED WHERE THE PARTS ARE IN ONE HAND, in flow_pick and across ONE call: the four are
   raised on the statement after `g_scan_weights[why]++` with no branch between them, so their delta over one
   loop must EQUAL that counter's delta. Two counters maintained by two statements, which is what makes it a
   check rather than a sum compared with its own summands. It is also the one thing that catches the way this
   pair rots — a `continue` introduced between the weighing and the block, which is the same correct-by-
   ADJACENCY shape flow.c's `sub_born++` and the `sub_offset` written beside it already stand on.
   THE SECOND HALF OF THAT ANALOGY USED TO BE `sub_gone++` AND IS REWRITTEN RATHER THAN DELETED, BECAUSE THE
   RETIRED READING IS THE INTUITIVE ONE AND THIS IS THE FOURTH SITE THAT CARRIED IT. A departure obviously
   changes a bucket, so it looks like it must change what every member of that bucket is worth — and it did,
   while flow_branch_bonus divided by the live gauge `sub_born - sub_gone`. That denominator is the LIFETIME
   mint count now, which no departure can move, so `sub_gone` reaches no term of flow_weight and a departure
   re-ranks nobody; flow.c's own walk over this invariant is where the correction is argued. What
   acct_depart's adjacency IS still load-bearing for is the census's membership identity, one scope over.
   IT DECIDES NOTHING, for the scan counters' reason exactly: no term of flow_weight reads any of the four, no
   pick branches on them, nothing is bounded by them.
   RETIREMENT: this goes when the ask no longer walks the frontier — the invariant is then held at the index's
   own update site, where it is not a disjunction and has nothing to be exempt from, so there is no arming
   left to count. */
/* THE WIDTH IS PART OF THE DECLARATION AND `long` WAS THE WRONG ONE — A 32-BIT ACCUMULATOR IS A STEP CAP
   ARRIVING AS A TYPE, WHICH IS THE ONE THING §NO BOUNDS FORBIDS AND THE ONE SHAPE NO REVIEWER READS AS A BOUND.
   These four and `flow_scan_weights` are raised ONCE PER MEMBER PER SCAN, so their unit is O(members × scans)
   and on a real page's frontier that is BILLIONS within the hour — while every other counter in this header is
   O(members) or O(steps) and is orders of magnitude from any wall. `long` is 8 bytes on the native host and
   FOUR on wasm32 (measured with both controls: `_Static_assert(sizeof(long)==4)` passes for `--target=wasm32`
   and `==8` fails), so the same source is unbounded on one host this project builds for and capped at 2^31 on
   the one that SHIPS. That is worse than a bound, because it is a bound only the vehicle has, and the native
   gates can never see it.
   MEASURED, AND IT IS NOT A HAZARD BUT A REPRODUCED ABORT: a real app page driven on the shipped artifact died
   at 19m13s, 18m53s and again inside a 25-minute window, byte-identical frame list each time, at the DCHECK in
   solver/result.c that compares exactly these four against those four scan weights. One census before the
   abort the right-hand side stood at 2,147,352,163 — ONE HUNDRED AND THIRTY-ONE THOUSAND short of INT32_MAX —
   with the engine weighing tens of thousands of members a second. It wraps negative, `LHS <= RHS` turns false,
   and the assert fires. The IDENTITY IS CORRECT and is not what was wrong; signed overflow is UB, so this
   assert is the OBSERVED behaviour of the defect and not the defect.
   AND THE CONSEQUENCE IS THE PRODUCT'S, NOT THE INSTRUMENT'S: every long real-page run ended at ~19 minutes,
   inside the incremental-snapshot path, at a working set of 357-429 MiB — so the 512 MiB RAM floor that is one
   of the two routes to the cross-session store was not merely unreached, it was UNREACHABLE, and the razor read
   ZERO on every one of those runs.
   SO THE RULE IS A WIDTH AND IT IS ASSERTED RATHER THAN ARGUED: a counter raised per member per scan is a
   FIXED-WIDTH 64-bit type, and the static assertion below is what makes a future `long` here a compile error on
   the host it would cap rather than a nineteen-minute abort nobody connects to a declaration.
   RETIREMENT: this record goes when no lifetime counter in this header is declared `long` at all, because the
   width is then not a thing a reader has to get right per field. */
typedef struct {
    int64_t armed;       /* comparisons the check actually MADE — the row that scores its predicted absence */
    int64_t stale_gen;   /* exempt: the frontier generation moved since this member was last weighed */
    int64_t first_seen;  /* exempt: this member had never been weighed, so there was nothing to compare against */
    int64_t running;     /* exempt: it held the thread, which is the one writer that may move its own half */
} FlowKeyChecks;
_Static_assert(sizeof(((FlowKeyChecks *)0)->armed) >= 8,
               "a per-member-per-scan lifetime counter is narrower than 64 bits — on the host where that is "
               "true it is a STEP CAP, which §NO BOUNDS forbids, and it fires as a wrapped comparison in "
               "solver/result.c rather than as anything a reader would connect to this declaration");
FlowKeyChecks flow_key_checks(void);

/* …AND WHETHER AN INDEX OVER THAT KEY WOULD HAVE RETURNED THE SAME MEMBER THE COMPARATOR DID, WHICH IS THE
   ONE QUESTION THE PAIR ABOVE DOES NOT REACH AND THE ONE A SUB-LINEAR ORDER IS UNBUILDABLE WITHOUT.
   flow.c's flow_pick says an index over this key "decides WHICH members can be the maximum and the exact
   comparison stays flow_weight's, or it has changed the answer".  The check above scores whether the key
   STANDS STILL; this scores whether the key ORDERS — two different claims, and only the second is what a
   candidate set rests on.

   WHY IT CANNOT BE SETTLED BY ARGUMENT, WHICH IS WHY IT IS A COUNTER AND NOT A PARAGRAPH.  In exact real
   arithmetic the surrogate is the order: within one account the reward and the family notch are common, so
   ordering by `member_key - carry*Q` is ordering by flow_weight, and that is the whole of the decomposition
   `notch = k + K + (p + R >= S)`.  In FLOAT it is not an identity, and the reason is the summand ORDER
   rather than the terms: flow_nonreward evaluates `((qn + opt) + dist) + bb` with the only VARYING term
   FIRST, so every partial sum depends on it and the stable tail cannot be factored out.  A surrogate is
   therefore a RE-ASSOCIATION, which flow.c's own words say "would differ from it in the last bit and reorder
   two members the order currently ties".  The two available repairs are both refused here: an epsilon band
   is not this file's idiom and cannot be made exact, and re-composing flow_weight so its member half is a
   SUBEXPRESSION is an ORDER change in the last bit and is a decision rather than a diff.

   SO THE QUESTION IS ASKED OF THE RUN INSTEAD, WHICH IS WHAT THIS PROJECT DOES WITH A CLAIM NO ARGUMENT
   SETTLES.  flow_pick folds the surrogate over exactly the population and exactly the tie-break its own
   comparison uses, and COUNTS what the two spellings did — whether they named one member, and where they did
   not, which of the surrogate's two modes it was.
   THIS READ `flow_pick … asserts that the member the surrogate picks carries the weight the comparator called
   maximal.  THE ASSERT IS THE POINT AND IT CAN FAIL`, AND IS REWRITTEN RATHER THAN DELETED BECAUSE THAT IS
   THE DESIGN A READER RE-DERIVES FROM THE PARAGRAPH ABOVE IT.  That equality is gone.  It asked the two
   spellings to agree BIT FOR BIT while this header's own flow_index_margin DECLARES them free to stand a
   derived distance apart, so it could not hold on any frontier large enough to present a candidate pair — it
   was an EXPECTATION with a `DCHECK`'s costume, and its cost was a run's whole emitted surface discarded
   after it had been earned.  Measured over the four real-page fires on record, every weight reproduced to the
   bit from the operands the line printed: the pairs stood 1 to 4 ulps apart and the declared margin was 12 to
   68 times that, so no fire ever refuted the surrogate, the margin or the order.
   A FIRE ALSO PROVED THE SINGLE TOP KEY UNUSABLE AND SAID NOTHING WHATEVER ABOUT AN INDEX, which is the other
   half of the retired reading and is why its removal costs no claim: flow_pick's band walk — a candidate set
   carrying flow_index_margin, needing no edit to flow_weight — runs on the SAME scan and ASSERTS that it
   returns the comparator's own extremum, and that assert stays.  Measured over every fire on record, three on
   the smoke fixture and four on real application pages: the band walk's two assertions and the per-member
   margin bound were SILENT on the very scan that aborted, which an abort being first-past-the-post makes a
   witness rather than a coincidence — the design this header offers as the answer was observed working
   exactly where the single top key failed.  The re-composition question survives — flow_pick's banner says
   what it decides, and it is TIE IDENTITY rather than buildability — and it is now decided from ROWS over a
   whole run instead of from the one pair that ended one.
   RETIREMENT: this record goes when the TIE IDENTITY decision has been made by the project owner, because the
   two rows below exist to inform exactly that and nothing in this file can discharge it.

   READ THE PAIR AND NEVER EITHER ALONE — the same shape as `armed` against its three exemptions.  `asked` is
   how many scans made the comparison at all, so it is the reachability witness without which a zero
   `differed` is satisfied identically by agreement and by a walk that never ran.  `differed` is the subset in
   which the surrogate and the comparator named DIFFERENT MEMBERS, which is the ordinary state when two
   members tie — informative, not a defect, and the row that says the check is examining anything at all
   rather than comparing a pointer with itself.  `differed_tie` and `differed_strict` PARTITION it by the one
   pair that states the mode, and reading either of them without the total is reading a numerator: a strict
   count that is a large SHARE of the differed total is the single top key naming a member the comparator calls
   worse repeatedly, and the same count beside a far larger tie count is the order being tied.
   …AND THE PAIR BENEATH THEM IS WHAT THE ANSWER TO THAT QUESTION COSTS, WHICH IS A DIFFERENT QUESTION AGAIN
   AND THE ONE THAT DECIDES WHETHER AN INDEX IS WORTH HAVING AT ALL.  The two rows above say whether the key
   ORDERS.  Where it merely TIES — which flow.c's own abort separates by reading `sur_w` against the
   SURROGATE'S reading of the member the comparator returned, and NOT against `bw`, which is the retired pair
   that named the wrong mode on four fires out of four and whose table flow_pick carries — and
   which is this frontier's ordinary state — the design that answers it needs no edit to flow_weight: take a
   CANDIDATE SET of every member within a derived margin of the surrogate's extremum and re-compare the
   survivors through flow_weight itself.  flow_index_margin derives that margin from the two expressions and
   flow_pick's band walk proves the set contains the comparator's own extremum, so the answer is the full
   scan's answer pointer for pointer.  What NOBODY had measured is HOW BIG THAT SET IS, and an index that has
   to re-compare most of the frontier has moved the walk rather than removed it.
   READ THEM AS A FRACTION AND NEVER THE NUMERATOR ALONE.  `band_members` is how many members the candidate
   set admitted; `band_weighed` is how many the band test was applied to, raised on the same walk over the
   same population, which is what makes their quotient the share of the frontier a re-compare would cost
   rather than a number over a denominator somebody supplied.  A SMALL share is an index that narrows.  A
   share near one is an index that saves nothing — and that is a finding about whether the surrogate is worth
   keeping, not a defect to repair.
   A LARGE SHARE IS EXPECTED AND IS NOT A WIDE MARGIN, WHICH IS THE ONE WAY THIS ROW WILL BE MISREAD — and
   the reason is a theorem rather than a measurement, so it is written here where the number is published
   instead of being left for a reader to re-derive from a figure that surprised them.  THE BAND CONTAINS
   EVERY MEMBER TIED WITH THE MAXIMUM, always: if `W(a)` equals the comparator's extremum then, with `M` the
   derived margin and `m*` the surrogate's own pick, `S(m*) <= W(m*) + M <= W(a) + M` (because `a` carries
   the extremum) and `S(a) >= W(a) - M`, so `S(m*) - S(a) <= 2M` and `a` is inside the edge.  The band's
   share is therefore AT LEAST the share of the frontier standing at the top weight, whatever the margin is
   — widen it and nothing changes, narrow it and the tie set is still in.  flow.c records this frontier as
   "73-93% of members tied at the top", so a band near that is the ORDER being tied and not the bound being
   loose, and a reader who responds to it by shrinking the margin has repaired the one thing that was not
   wrong.  What such a reading DOES say is the thing the row exists for: an index cannot beat the tie, so a
   candidate set on a tied frontier re-compares most of it and the walk is moved rather than removed.
   THE MARGIN IS NOT THE DIAL, IN OTHER WORDS, AND THE SHARE IS A PROPERTY OF THE ORDER.  The only reading
   that would indict the bound is a band much LARGER than the tie set — which needs the tie set measured
   beside it and is a second question this pair does not answer.
   BOTH ARE LIFETIME COUNTS SUMMED OVER ASKS and may be differenced; neither is a gauge, neither is
   per-member, and neither may be read against `members`.  `index_asked` remains the reachability witness for
   all five rows beneath it: a zero band, or a zero differed, beside a zero ask is a fold that never ran.
   ALL SIX ARE LIFETIME COUNTS, raised under APICLIENT_DEV, and none decides anything: no term of flow_weight
   reads any of them, no pick branches on them, nothing is bounded by them.
   RETIREMENT: these rows go when the ask no longer walks the frontier — the index is then the thing being
   asked and its agreement with flow_weight is held at its own update site, so there is no fold left to
   count and no band left to price. */
typedef struct {
    long index_asked;      /* scans that folded the surrogate and had a maximum to compare it against */
    long index_differed;   /* …of those, the ones where the surrogate named a DIFFERENT member. IT TESTS NO
                              WEIGHT, so `of equal weight` — which this comment said — is true of no scan in
                              particular: the pair it counts may weigh the same or differ in the last bits,
                              and since no abort stands here any more the run continues either way. The two
                              rows below are what separates those, and they were this field's own next-diff
                              clause */
    long differed_tie;     /* …of THOSE, the ones where the surrogate reads the comparator's own member AT its
                              own extremum — it lost a distinction the comparator makes rather than making a
                              different one, which is the arm a margin-carrying candidate set answers with
                              flow_weight untouched */
    long differed_strict;  /* …and the ones where it reads that member on the LOSING side of its extremum — a
                              real disagreement, which is the arm the TIE IDENTITY decision is about. The pair
                              is read against `index_differed`, which it partitions, and flow_pick asserts the
                              sum because the three are raised by two statements over one condition */
    /* AND THE WIDTH IN THIS STRUCT IS THE UNIT, WHICH IS WHY THE TWO BELOW ARE WIDER THAN THE FOUR ABOVE AND
       NOT AN INCONSISTENCY SOMEBODY WILL TIDY. The four `long` rows are raised ONCE PER ASK, so they count
       PICKS; these two are raised inside the band walk, so they count MEMBERS × PICKS. The derivation is the
       raises themselves and is three greps: `index_asked++`, `index_differed++` and the `differed_tie` /
       `differed_strict` pair all sit outside the member loop, and `band_members += band` / `band_weighed +=
       band_of` sit in it. On a real page's frontier — measured 18,724 and 25,650 members — that is four orders
       of magnitude between the two groups, which is the whole of why only one group reached a 32-bit wall. */
    int64_t band_members;  /* members the derived-margin candidate set admitted, summed over those scans */
    int64_t band_weighed;  /* …and the members that set was tested over — the denominator of the row above.
                              PER MEMBER PER SCAN, so 64-bit for FlowKeyChecks' reason: measured 23.6 M from
                              the same 2^31 wall as the counters that reached it. */
} FlowIndexChecks;
_Static_assert(sizeof(((FlowIndexChecks *)0)->band_members) >= 8
                   && sizeof(((FlowIndexChecks *)0)->band_weighed) >= 8,
               "a per-member-per-scan lifetime counter in FlowIndexChecks is narrower than 64 bits — the two "
               "band rows share FlowKeyChecks' unit and therefore its wall, and the comment above says which "
               "rows of this struct are which so the answer is read rather than guessed");
FlowIndexChecks flow_index_checks(void);

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
/* THE FRONTIER'S ARRIVAL AND DEPARTURE PROCESSES, for the life of this instance — see the census rows above
   for the reading, the identity and why `flow_rank_changes` cannot answer this. */
int64_t flow_arrivals(void);
int64_t flow_departures(void);
/* …AND THE THIRD ARM OF THAT DEPARTURE TOTAL: how many members were still standing when this instance's
   registry went down. The other two arms are engine.c's (`finished`, `sold`) and live in the frontier census,
   so the three are only ever in one hand there — which is where the partition is asserted and why this is
   exported rather than kept private to flow.c. A departure that credits no arm is a fourth exit from the
   frontier, and that assert is the only thing that can see one. */
int64_t flow_departures_teardown(void);

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
   with re-dispatches that pass over a starved member, which are the defect.
   AND THIS ROW DOES NOT SEPARATE THOSE TWO EITHER, WHICH THE SENTENCE THAT STOOD HERE CLAIMED IT DID ("This
   counts only the second"). The condition is `best` having been dispatched before, and being dispatched
   before says nothing about having anything to continue: a framed flow re-picked to finish its program while
   an arm it forked stands at its exact weight is counted here, and on a forking page that is the ordinary
   shape of every quantum of every multi-quantum program, because an arm is born at its parent's weight. The
   claim was refuted by a CONSUMER of the row, which had declined to print the quotient for exactly this
   reason while the two sites that declare it said the separation had been made. `flow_starved_picks_idle`
   below is the separation; this row is the population it is a subset of, and kept because the REMAINDER — what a
   forking frontier spends finishing programs — is a reading in its own right. */
long flow_starved_picks(void);

/* THE WHOLE REBUILD AN EPOCH-KEYED INDEX OVER `flow_index_key` WOULD PAY, SUMMED OVER A RUN — a LIFETIME
   counter, published as `epochRebuildLifetime`, and the reading that decides whether a sub-linear order over
   this frontier is buildable at all. flow.c holds the derivation; the reading is this.
   THE DENOMINATOR IS `scanNextWeights` AND IT ALREADY SHIPS, which is why no new one is owed and why it is
   named here rather than left to be re-derived: that row is the lifetime sum over scans of the members each
   one weighed, i.e. exactly the walk an index would REPLACE. Well below it, the epoch is a COST and an index
   narrows; at or above it, the rebuild is the walk moved rather than removed and an index buys nothing.
   `epochResetsLifetime` is its own denominator for the other question — the quotient is the average rebuild
   per emission, which separates a large total over many cheap emissions from a small one over few expensive
   ones, and those take different diffs.
   A ZERO IS A CLAIM AND NOT A CLEAN BILL, so read it against `epochResetsLifetime` first: zero resets is a
   run that never emitted, and the row is then silent about the design rather than favourable to it. Zero
   rebuild across NON-ZERO resets is the strongest possible result — every emission found the whole family
   already at base — and it is reachable, because the population empties whenever nothing has been charged
   since the last finding.
   RELEASE-LIVE, unlike the key and index stamps beside it: the maintenance is four O(1) statements rather
   than a per-member evaluation per scan, so this is readable off the artifact the product actually ships. */
long flow_epoch_rebuild(void);

/* …AND HOW MANY EMISSIONS RESET A FAMILY'S BASE — a LIFETIME count, published as `epochResetsLifetime`, and
   the denominator of the row above. Raised on the same statement group as that sum and nowhere else, so the
   two cannot come to describe two different populations and no reader can take an average from one of them
   alone. */
long flow_epoch_resets(void);

/* …AND THE SUBSET OF THOSE IN WHICH THE MEMBER RE-DISPATCHED HAD NOTHING TO CONTINUE — no live frame and no
   microtask checkpoint owed, which is the unit boundary HTML §8.1.4.4 "Calling scripts" step 3 of clean up
   after running script draws and the one `visits` is credited at. A member standing there has FINISHED its
   trial, so handing it the thread again while a member that has never had one stands at its exact weight is
   §scheduler's razor's STARVES with no necessity behind it; the remainder of the row above is a program being
   finished, which a tie must not interrupt.
   THE FRACTION IS OF `picks_lifetime`, exactly as the superset's is, and the two are raised under one
   condition at one line so `idle <= starved` holds by construction and neither is a reading of a second
   moment. A LIFETIME COUNTER and one of the few kinds a reader may difference.
   IT ASKS ALL THREE CLAUSES OF THE BOUNDARY NOW, AND IT IS NO LONGER AN UPPER BOUND. engine.c's unit
   boundary is `!frame && !JS_HasParkedFlow(runtime) && !flow_job_microtask`; `flow_between_units` asks the
   first and the third and flow.c's `flow_holds_park` asks the second, so a member with no frame, no microtask
   owed and a PARKED CONTINUATION is no longer counted here as though it had finished its trial.
   WHAT MADE THE SECOND CLAUSE ASKABLE FROM HERE IS WHO IS COUNTED, NOT A NEW HANDLE. The raise requires
   `best != seed`, the incumbent SEEDS that scan and can never be the member counted, so every member this row
   counts is a NON-RUNNING one — and a non-running member's park queue is on `Flow::parked`, exactly where the
   predicate reads it. Only the RUNNING member's queue is in the runtime, and asking about that member is an
   assert rather than a silent NULL.
   THE READING THIS BUYS IS THE ONE THAT MATTERS AND IT IS WORTH SAYING WHY: an upper bound NEAR ZERO is
   decisive and an upper bound that is LARGE is not, so while this row was a bound it could not be trusted in
   the direction an ordering question is actually asked in. */
long flow_starved_picks_idle(void);

/* HOW DEEP THE TIED PLATEAU IS — FOUR LIFETIME ROWS THAT SEPARATE §Attention'S VALUE YIELD FROM §scheduler'S
   NEVER-STARVED GUARANTEE, WHICH RESOLVE OPPOSITELY ON ONE POPULATION AND WHICH NOTHING IN THIS FILE COULD
   TELL APART. The two sentences are not in conflict and the dilemma dissolves on enumerating what each is
   about, which is the thing to read first:
     §Attention's yield is about an incumbent the order can SEE is top-ranked — "a top-ranked flow runs on at
       ~zero switch cost". A RETENTION is that, and it is correct.
     §scheduler's optimism bonus is about a member ranked BEHIND — "so a never-run flow is never starved".
       A member TIED with the maximum is in neither population: nothing is ranked ahead of it, so there is no
       starvation to cure, which is the argument `never_picked` above already makes in its own words and the
       reason relaxing flow_pick's comparison would implement nothing.
   SO THE ONLY THING EITHER SENTENCE CAN BE WRONG ABOUT IS HOW LONG A RETENTION LASTS, and that is the one
   quantity neither of them states and no row here was measuring. flow_pick's own banner named it — "how often
   the incumbent kept the thread while a never-run member stood level with it … belongs to a reading about
   PLATEAU DEPTH" — and left it uncounted until the diff that added these four rows, which is where that
   banner's own clause is rewritten.
   WHAT THE SPECIFICATION PREDICTS, SO THAT THESE ROWS CAN REFUTE IT RATHER THAN ILLUSTRATE IT: the aging term
   above claims in its own words that "a flow tied with an unrun sibling on reward and bonus hands over after
   ONE quantum, which is the queue rotating". flow_age_running is called unconditionally by the dispatch loop
   and its charges TELESCOPE (engine.c carries each turn's clock reading into the next turn's `t0`), so the
   incumbent's own silence accumulates the whole slice and outruns a level waiter's notch — engine.c asserts
   that observability at the charge. The prediction is therefore a BOUND ON DEPTH and not the existence of a
   handover, and the quotient below is what scores it.
   READ THEM AS TWO FRACTIONS AND NEVER A NUMERATOR ALONE:
     `plateauHeld / plateauAsked`   how tied the frontier is at the line that dispatches — the share of
                                    makeable comparisons the tie-break decided in the incumbent's favour. It
                                    is the ORDER being tied and is not by itself a defect.
     `plateauHeld / plateauRuns`    THE DEPTH, and the reading all four rows exist for. Bounded is the queue
                                    rotating, which is the specification holding. `plateauRuns` at 1 beside a
                                    large `plateauHeld` is ONE unbroken hold for the whole run, which is the
                                    guarantee being FALSE and is the only reading here that asks for a diff.
   `plateauAsked` IS THE REACHABILITY WITNESS AND NONE OF THE OTHER THREE MAY BE READ WITHOUT IT, the same
   shape `index_asked` takes for the six `FlowIndexChecks` rows: a zero `plateauHeld` beside a zero ask is a scan
   population that never existed — no incumbent the scan weighed, or no never-dispatched member to compare it
   against — and is satisfied identically by a frontier the order is serving perfectly and by a dispatch loop
   that never ran. A zero ask beside a non-zero `picksLifetime` is itself a finding: it says the frontier held
   no never-dispatched member at any dispatch, which is a frontier that drains.
   ALL FOUR ARE LIFETIME COUNTS, RELEASE-LIVE, and none decides anything: no term of flow_weight reads any of
   them, no pick branches on them, nothing is bounded by them. `plateauHeldIdle` is an UPPER BOUND by TWO of
   the unit boundary's three clauses — one unaskable of the running member at any price, one affordable only
   as an O(1) predicate this file does not yet have, since the row fires on nearly every scan of a tied
   frontier. See the residual at its counter in flow.c for both and for what closes them.
   NAMED RESIDUAL. NOT COVERED: an automatic reading. `engine/build.mjs`'s @WFQ verdict is what turns this
   census into a sentence — it is where "a FRONTIER ADVANCING WITHOUT RETIRING" comes from — and it says
   nothing about the depth, so the two quotients above are computed on every census of every run and read only
   by somebody who goes looking. WHAT THE NEXT DIFF BUILDS: that verdict clause. It is NOT built here and the
   reason is the whole of why this is a residual rather than an omission: a verdict has to say whether a depth
   is BOUNDED, that is a BAND, and no depth has ever been measured — so the band would be a number nobody
   derived sitting in the one place a reader takes a number from, which is the proposed-narrowing defect with a
   threshold in place of a rule. The first measured depth is what the band comes from and the clause lands with
   it. HOW ITS ABSENCE SHOWS: a run whose verdict names a frontier that advances without retiring while the
   document beside it carries `plateauRuns` at 1 and `plateauHeld` in the thousands — the verdict silent about
   the one row that says whether the ordering is the cause.
   RETIREMENT: these rows go when the depth is held by an assertion instead of a reading — an upper bound on
   consecutive retentions derived from FLOW_AGE_QUANTUM and the slice, asserted where the charge meets the pick
   as engine.c's own notch check already is — because the quotient is then a claim the build refuses to break
   rather than a number somebody has to go and read. */
long flow_plateau_asked(void);
long flow_plateau_held(void);
long flow_plateau_runs(void);
long flow_plateau_held_idle(void);

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

/* …AND THE TWO PIECES OF IT THE PAIR ABOVE DOES NOT ACCOUNT FOR, WHICH IS A CORRECTION TO THAT SENTENCE AND
   NOT AN ADDITION TO IT. `flow_silence_notch` is `(own + fam) / S` and the two notches above are `own / S` and
   `fam / S`; integer division of a SUM is the sum of the divisions PLUS A CARRY, so the three rows a reader
   has do not reconcile and the missing bit is a WHOLE NOTCH — one FLOW_AGE_QUANTUM, 0.012 points, against a
   whole-order spread measured at 0.030 and 0.036 on two documents.
   THEY EXIST TO PRICE AN ASK, NOT TO ORDER ANYTHING. No term of flow_weight reads either, no fork carries
   either, and flow_weight's own arithmetic is untouched by their existence: the split says that between two
   frontier generations the ONLY per-member quantity in the weight that moves is this carry, whose threshold is
   COMMON to the family and sweeps monotonically — so members cross it in descending order of their phase and a
   maximum over the frontier is a maximum over two CONTIGUOUS RANGES of it. flow.c states the derivation, and
   flow_pick's own "why this is still a walk at all" is where the claim it corrects lives.
   RETIREMENT: this pair goes when the ask no longer walks the frontier, at which point an index is their
   reader and `sil_phases` below stops being a row. */
int64_t flow_silence_phase(const Flow *f);
int flow_silence_carry(const Flow *f);

/* …AND THE ONE SUMMAND THAT CLAIM DOES NOT COVER, PUBLISHED SO THAT IT CAN. The pair above says that between
   two frontier generations the ONLY per-member quantity in the weight that moves is the aging's carry, and
   engine.c's rival assertion is where that is checked rather than asserted in prose — over the rival's own
   service notch, its completed-unit count, its reward and its fitness distance. flow_weight has a FIFTH
   summand: flow_branch_bonus, `1.0 / sub_born` over the member's top-level arm, and `sub_born` is a bucket
   field every member of that arm reads through one pointer — so ONE fork raises it for ALL of them at once.
   Nothing was watching it, and the reason was reach and not exemption: `FlowAcct` is file-private to flow.c,
   so the term had no spelling outside that file for an assertion to name.
   IT PUBLISHES THE DENOMINATOR AND NOT THE TERM, WHICH IS THE STRONGER OF THE TWO AND THE ONLY EXACT ONE.
   `1.0 / sub_born` is a pure function of this integer, so equality here ENTAILS equality of the term, while a
   double would be compared with `==` against a quotient recomputed at another instant — the last-bit
   comparison flow.c's own phase pair refuses for an index, refused here for the same reason. It is also what
   a reader can reconcile against `brBornMax`/`brBornMin`, which are this field over the whole frontier.
   IT IS NOT A TERM AND NOTHING RANKS BY IT: flow_weight is untouched, no fork carries this and no arrival
   copies it. Its one reader is an assertion about what may move while the generation stands still.
   RETIREMENT: this goes with the pair above and for its reason — when the ask no longer walks the frontier,
   the index is what holds this invariant and holds it at its own update site. */
long flow_branch_born(const Flow *f);

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

/* IS THIS ONE MEMBER OUT OF THE RUNNABLE ORDER — flow_pick's own filter, asked of a single flow. The pick
   skips a marked member and nothing else skips anything (no seed and no exclusion on the runnable entries),
   so this predicate answering FALSE for a live member is exactly the statement "the scan weighed it", and
   that is what a caller comparing its own incumbent against the scan's maximum needs in order to know
   whether the two readings are about the same population. It is published for that one consumer rather than
   because it is generally useful: the alternative is the caller inferring the mark from the step code that
   laid it down, which is a second writer's worth of knowledge about a mark that has exactly one writer. */
int   flow_host_owed(const Flow *f);

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

/* THE IMAGE THIS MEMBER OWES THE HOST — @PERWORLD, and the whole of what it means is on `paint_owed` above.
 * The set is written on TWO ROADS, which are this entry's own call sites and are what
 * `engine_request_paint_every_world` is made of: `paint_mark_standing_members` marks the members ALIVE at the
 * ask, which only a walk of the frontier can name, and `flow_new` marks the members NOT YET BORN, which only
 * the mint can. EITHER ROAD ALONE LEAVES A POPULATION UNMARKED — the walk misses every arm forked afterwards,
 * and the mint misses the boot flow, which on a document that has not stepped is the entire frontier and the
 * only member with a document in it. Each mark is discharged by the scheduler HANDING THE THREAD BACK with
 * that member switched in, which is the one moment its pixels exist.
 *
 * IT IS NOT A HOST-OWED MARK AND MUST NOT BE FOLDED INTO ONE, which is the near-miss this pair invites. A
 * host-owed mark says the member CANNOT PROGRESS and takes it OUT of the pick; this says the member has
 * something the host wants to see FIRST and changes the pick not at all. Folding them would put every member
 * of an asked frontier out of the pick at once, which is the STALL, and the host would be handed a frontier
 * reporting that it owes replies it does not owe. */
void  flow_set_paint_owed(Flow *f);
void  flow_clear_paint_owed(Flow *f);
int   flow_paint_owed(const Flow *f);
/* EVERY WORLD MINTED FROM NOW ON OWES THE HOST ONE IMAGE — a standing MODE and not a second ask, which is the
   distinction the two entries exist to keep: `flow_set_paint_owed` marks ONE member a caller is holding, and
   this decides what a member is BORN with. It is one-way on purpose. A host turns it on because it wants one
   picture per timeline for the whole run, and a mode that could be turned off would let a run write a
   world-named image for some arms and not others with nothing in the artifact to say which — the plausible
   datum §A-FIELD-A-CONSUMER-DEFAULTS names, arriving as a directory of pictures that reads as a complete set
   of a document's worlds and is a sample of them. The members ALIVE when a host asks are not this entry's
   business: `engine_request_paint_every_world` walks those, and this covers the ones not yet born. */
void  flow_paint_every_world(void);

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
/* A CANDIDATE HAS REPLAYED PART OF ITS OWN RECORDED PATH — rung zero, the runway, and the observation §@S(i)
 * requires that no site in this engine was making. `consumed` is the arms this run has replayed and `total` the
 * recorded path they are a prefix of; the reading is their ratio and NOTHING is accumulated. Called from
 * decide.c's dec_replay, one arm at a time, each strictly before the source read the delivery reports at.
 * A READING AND NOT A PAYMENT, so it is written rather than added and says the same thing however many times it
 * is taken — the distinction flow_observe_survival's banner draws between a comparator and a ledger, and the
 * one that makes a fitness usable at all. */
void  flow_observe_replay(Flow *f, long consumed, long total);

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
/* THE RUNNING FLOW JUST DETERMINED A SOURCE'S VALUE ON AN ARM ITS OWN EXAMPLE CONTRADICTS — `path_pinned`'s ONE
   writer, and the whole of what separates a request whose ADDRESS may rest on a witness this engine chose from
   one merely built past a forced gate. Idempotent and monotone for `flow_mark_forced_arm`'s reasons.
   IT IS CALLED ONLY WHERE THAT FUNCTION HAS JUST BEEN CALLED, which is what makes the nesting structural
   rather than a second copy of the forced test: decide.c's `decide_note_forced_arm` ANSWERS whether it marked
   the path, and this is reached only on that answer. Two sites spelling "did this arm contradict its example"
   would be two rules free to disagree, and the disagreement would file a chosen witness under a path that
   denies standing on one. */
void  flow_mark_pinned_value(void);
/* HAS THIS FLOW DETERMINED A VALUE ON SUCH AN ARM — read at the PARK, beside `flow_path_forced` and for the
   same reason: a park is a work item and §scheduler's "an operation that becomes a work item takes its inputs
   with it" applies to what its address RESTS ON exactly as it applies to the address. A flow that parks a
   request and pins a source afterwards built that request on the path it had THEN. */
int   flow_path_pinned(const Flow *f);
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
 * AND IT IS UNSTARTED BECAUSE §7.5.10 STEP 5 IS. "Remove any tasks whose document is document from any task
 * queue (without running those tasks)" is about work that has not run; a row the flow has already compiled is
 * a program it may be SUSPENDED INSIDE, and the standard has no object at all for a continuation suspended
 * mid-program — that one is the flow's own state and §NO BOUNDS forbids touching it. `last_compiled` is the
 * line between them, because it is what the compile site advances and asserts against.
 *
 * PURE: no allocation, no JS value touched, no reference taken, so a DCHECK may ask it. */
int   flow_programs_unstarted_for_document(const Flow *f, uint32_t doc);

/* TAKE THEM OUT — HTML §7.5.10 "Destroying documents"' step 5, "Remove any tasks whose document is document
 * from any task queue (without running those tasks)", performed on the queue the runtime's own job walk cannot
 * see. Returns how many rows went, which is exactly what the count above answered one instant earlier.
 *
 * WHY THIS QUEUE IS ONE STEP 5 IS ABOUT. `JS_DropJobsForContext` empties the runtime's job queues, and a
 * document's SCRIPTS are not in them: they are rows of the running flow's one program sequence. A row is a task
 * by the standard's own reckoning — §8.1.4.4 "Calling scripts" runs it and §4.12.1.1 "Processing model" queues
 * it ("queue an element task on the DOM manipulation task source") — so a row of a destroyed Document left
 * behind is page script that will be compiled into a realm whose browsing context is null.
 *
 * IT IS THE RUNNING FLOW'S ROWS AND NOT THE FRONTIER'S, for the reason the count above is per flow: the
 * destruction is per timeline, and a sibling arm that has not destroyed this document is running a document
 * that is still there. Taking its rows would destroy something in a timeline that never asked.
 *
 * IT ALSO REPAIRS THE ABSOLUTE POSITIONS THAT ARE LEFT, which is the half a caller must not try to help
 * with: the flow's own `imm_at`/`imm_next` are absolute, and a compaction that moved rows without them would
 * order a later interposition against a run that is no longer where it was. `script_i` and `last_compiled`
 * are provably unaffected — see the note at the end of the body.
 * THE PENDING REGISTER NEEDS NO REPAIR AND THAT IS WHY THIS FUNCTION NO LONGER ABORTS. An entry names its
 * row by `dyn_id`, not by position, so a surviving row keeps its name across the compaction however far it
 * moves; what the walk still owes is to DROP the entries whose rows are going, which is HTML §7.5.11
 * "Aborting a document load" step 2's "Cancel any instances of the fetch algorithm in the context of
 * document". The abort that stood there refused a removal below an outstanding external script because the
 * entry's position could not be corrected — the record is SHARED with every forked arm and only this arm
 * destroyed the document, so one field would have had to hold two positions at once. A name is the same in
 * both arms, so there is nothing left to correct.
 *
 * NOT PURE: it frees rows and mutates the register, so unlike the two counts above it may NOT be asked from
 * inside a collection and may not stand in a DCHECK. */
int   flow_programs_remove_for_document(Flow *f, uint32_t doc);

/* WHICH ROW OF `f`'s SEQUENCE IS CALLED `id`, or -1 when this flow holds none — see `dyn_id`.
 * PURE, so it may stand inside a DCHECK, and it ASSERTS NOTHING ITSELF: three call sites reach it (the two
 * halves of a document script's delivery and §7.5.10's removal walk) and a should-never-happen checked here
 * would report THIS line for all three, which CLAUDE.md names as an assert whose remedy has no site. Each
 * caller tests the answer and says what its own -1 would mean. */
int   flow_dyn_row_by_id(const Flow *f, uint64_t id);

#endif
