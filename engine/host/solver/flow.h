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
    int   script_i;        /* position in this flow's one program sequence: a row of `dyn`, on [0, dyn_n) */
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
    int   imm_next;        /* the slot the next immediate row of that run takes */
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
    void *delta;           /* this flow's isolated heap COW delta (CowDelta*), applied while running */
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
    int64_t picks_live;       /* gauge: dispatches held by the members standing now */
    int64_t picks_max;        /* gauge: the most any one of them holds */
    int64_t picks_lifetime;   /* lifetime: every dispatch this instance has made, departed members included */
    /* lifetime: the dispatches departed members took with them (flow.c's `g_picks_departed`, raised at
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
    long br_live_max;   /* gauge: the most live members in one bucket — the fat side of a branch */
    long br_live_min;   /* gauge: the fewest; see above for why the root's bucket usually owns this */
    long br_live_sum;   /* gauge: their sum, published because `== members` is the partition identity */
    long br_born_max;   /* lifetime: the most members ever minted into one live bucket */
    long br_born_min;   /* lifetime: the fewest — the two ends of flow_branch_bonus's own denominator */
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
    long br_crowd_live;    /* gauge: live members in the bucket that owns `br_live_max` */
    long br_crowd_born;    /* lifetime: that same bucket's own mint count */
    int64_t br_crowd_us;   /* lifetime microseconds: that same bucket's own receipt */
    /* The minter: the same three numbers for the bucket that has minted the most, a different arm whenever an arm
       shed what it minted (`sub_born = live + sub_gone`). A walk over an unknown length forks a `stop at n` arm
       at every position and each arm finishes, so the minter stands narrow and is not the crowd. It carries
       the smallest branch bonus, so `br_minter_us / br_held_us` against `br_minter_live / members` says whether
       the branch term's demotion reaches the thread (the crowd's three states, asked of this arm).
       `br_minter_gone` is the shed count, published nowhere else. flow_wfq_census asserts
       `br_minter_live + br_minter_gone == br_born_max` (different writers, as for the crowd);
       `br_minter_us == br_crowd_us` says the two selectors name one arm. Zero semantics are the crowd's. */
    long br_minter_live;   /* gauge: live members in the bucket that owns `br_born_max` */
    long br_minter_gone;   /* lifetime: arms that same bucket has shed — published nowhere else */
    int64_t br_minter_us;  /* lifetime microseconds: that same bucket's own receipt */
    int64_t br_us_max;  /* lifetime microseconds: the most thread time one bucket's subtree ever received */
    int64_t br_us_min;  /* lifetime microseconds: the least */
    int64_t br_us_sum;  /* lifetime microseconds: their sum — one half of the burn identity */
    /* `br_us_sum` split by whether anyone stands in the bucket, so a live arm's share has a live denominator
       (`br_held_us`). Two accumulators in two arms of one condition, so `br_held_us + br_empty_us == br_us_sum`
       is a check; asserted in flow_wfq_census. These rows are gauges despite the per-bucket "lifetime" labels:
       a bucket whose subtree wholly departs is freed in `acct_unref` and its receipt moves into
       `br_retired_us`, so `br_us_sum`, `br_held_us` and `br_empty_us` fall then, and only `br_retired_us` and
       `charged_us` are monotone (solver/result.c files them the same way). */
    int64_t br_held_us;  /* lifetime microseconds: received by buckets holding at least one live member */
    int64_t br_empty_us; /* lifetime microseconds: received by buckets still taken and holding none */
    int64_t br_retired_us; /* lifetime microseconds received by buckets whose subtree has wholly departed */
    int64_t charged_us; /* lifetime microseconds the scheduler has charged at all — the identity's total */
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
    long br_fan_max;    /* gauge: the most live members forked directly off one non-root node */
    long br_fan_sum;    /* gauge: live members forked off a non-root at all — the maximum's denominator */
    int br_fan_depth;   /* gauge: the fork-tree depth of the node holding that maximum; 0 when there is none */

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
    long dec_max;         /* the deepest decision vector of any member — the gate sequence's own length, which
                             is what the row above is a fraction of */

    /* The order itself, in the pick's units. `w_top` holds the front of the queue and `cand_w_max` is the best
     * any candidate offers against it, so their gap is the ordering question directly. A candidate records no
     * endpoints (endpoint_suppress), so its reward stays at its arrival coordinate until it fires and each
     * charge is pure penalty: a served candidate ranks below an unserved one at the rate FLOW_AGE_RATE names.
     * Aging is absolute, while the scheduler's demotion of a monopolizer is comparative. flow.c's
     * flow_arrive_at_virtual_time applies SFQ's `max{v(t), F_prev}` to from-baseline entries as a continuing
     * relation, so a created document or candidate is not placed ahead of the backlog.
     * Named residual. Not covered: the aging term itself is not relative to the frontier, so engine_top_weight
     * (Level 1) falls one point per second of unproductive CPU without bound. Next diff builds: a virtual-time
     * frame of reference for the aging term. Absence shows as: a mature document permanently outranked by every
     * newly booted page once it passes `(reward + 1)` seconds of silence; `svc_min`/`svc_fam_min` measure it.
     * Every field of this struct is emitted; flow_wfq_census's accounting assertion keeps that true. */
    /* The fitness term's range (@S distance, added by flow_weight). Its floor is the constant 0: a flow with no
       payload has no ladder and both fitness writers refuse to record one, so [0, dist_max] is the range. With
       `cands: 0` it is 0 by construction, which is a fact about the run's @S population. */
    double dist_max;
    double w_top;         /* flow_best's weight — what holds the front of the queue */
    double w_min;         /* the lowest weight in the frontier — the other end of the same order */
    double cand_w_max;    /* the best weight any @S candidate can offer against `w_top` */

    /* The two halves of the leader's own silence, read off the member flow_best returned, in notches like
       `svc_max`/`svc_fam_max` (multiply by FLOW_AGE_QUANTUM for points). A ledger is monotone, so a frozen
       `val_top` and a slowly earning one look alike; the silence is not monotone (flow_credit_emit zeroes
       `fam_us`), so these say whether the leader's aging is being forgiven.
       Reported separately: on a one-family frontier `top_svc_fam` is a common offset that cancels from every
       gap. `top_svc` is the half that moves a gap: a monopolising front sinks and `top_svc` climbs, while a
       front refilled by fresh arms keeps it low or sawtoothing while the same gap stands; those take opposite
       work (re-price the aging, or stop the mint outranking the tail). Not flow_silence_notch, which floors
       the sum, so the halves can differ from the weight's notch by one. Bounded by the extrema above (the
       leader is one of the members walked) and asserted. */
    int64_t top_svc;
    int64_t top_svc_fam;
    /* How many times the leading account's within-family order has been erased: its `emit_gen`, raised once per
       credited finding by flow_credit_emit and by nothing else. `val_top` cannot stand in for it, because a
       credit is any positive amount (the @S survival ratchet credits fractions).
       An emission zeroes both silence halves for every arm of the family at once, leaving arms in one visit tier
       exactly tied, so flow_pick returns the first maximum in registry (birth) order and the tier is swept from
       its oldest member, one member per quantum of own silence; the next emission restarts the sweep. If that
       holds, `picks_max` tracks this within a small factor and `picks_live / picks_max` is the mean sweep depth;
       `val_top / top_forgiven` says whether the leader earns findings or ratchets fractions.
       A per-account lifetime count whose series may fall at a leader change: both falling is a new leader,
       this falling while `val_top` rises is a generation going backwards. Read per sample, never differenced. */
    int64_t top_forgiven;
    /* The most every term of the order except the reward can lift one member: flow.c's FLOW_NONREWARD_MAX, the
       bound flow_nonreward asserts, emitted because it folds three terms (the optimism ceiling, the fitness
       ladder over FLOW_RUNGS_N, the aging's zero) and a reader's re-derivation would go stale. A member's weight
       is its account's reward plus its non-reward sum, so `(val_top - val_min) + nonreward_max` is the largest
       gap a non-negative non-reward sum can produce; a gap above it means the trailing member's own terms are net
       negative. On a one-family frontier the reward difference is zero and the bound is this row alone. */
    double nonreward_max;

    /* The job backlog, split by what each job waits on, over the predicates flow_step and flow_pick already ask;
     * a job that never runs is a route that cannot reach it or an order that has not got to it. Disjoint and
     * exhaustive (two booleans over every member):
     *   `jobs_owed`   host-owed mark, so flow_pick refuses it (`runnable_only`): it waits on the host.
     *   `jobs_framed` fails flow_stack_empty (HTML §8.1.4.4 "Calling scripts", clean up after running script
     *                 step 3), which every job arm of flow_step is under: a live frame or a DYN_POS_IMMEDIATE row
     *                 at the cursor, not `frame` alone. It waits on the member completing its unit of work, which
     *                 is benign only while frames end.
     *   `jobs_ready`  neither: it waits on rank alone. Its zero is two states, which `mem_unframed` separates: no
     *                 unframed member (frames are not ending), or unframed members holding no jobs.
     * Whether frames end is read from `stepUnitRuns`, summed against `steps`; no single arm is a rate, since
     * `resume-ended-its-frame` is the intersection of completions (`g_completed` also counts
     * `report-an-exception`) and frame clears (`f->frame == NULL` also follows `program-detached-its-base`). */
    /* `job_w_gap` is `w_top` minus the best weight any ready holder offers, in reward points: 0 means the top of
     * the order holds a runnable job; a figure on the scale of `val_max - val_min` is a backlog the aging cannot
     * reach in a session. Read beside `jobs_ready` (0 with no ready holder). `>= 0` by construction (`w_top` is
     * flow_best's maximum over the same members), and a DCHECK beside it catches the pick and census disagreeing.
     * `jobs_ready_task`/`jobs_ready_micro` split the ready row by which arm of flow_step can dispatch the job.
     * solver/engine.c's ladder puts the checkpoint arm (flow_checkpoint_due: `flow_job_microtask &&
     * flow_stack_empty`) above the program sequence and the task arm below it, as the `else` of the sequence
     * test. So a ready member's microtask is taken by the checkpoint arm outright, while its task waits for a
     * step that starts no program. All task and no micro measures the sequence arm's exclusion; any micro refutes
     * that diagnosis for those jobs (other arms above the checkpoint, or the pick, may still hold them).
     * Gauges. flow_wfq_census asserts `jobs_ready_task + jobs_ready_micro == jobs_ready` (the total sums queue
     * lengths, the halves walk records), and engine/build.mjs re-asserts it for release.
     * `vis_zero` counts members that completed no unit of work, the member-side twin of `jobs_framed`; their
     * optimism bonus cannot decay (flow_queue_weight keys it on completed units). */
    /* Named residual. Not covered: the pair is inferred from two gauges, not read off the decision. Next diff
     * builds: a census row stating which arm of flow_step declined a ready job when it declined it. Absence
     * shows as: a ready backlog attributed to the wrong arm of the ladder. */
    long jobs_ready;
    long jobs_framed;
    long jobs_owed;
    double job_w_gap;
    long jobs_ready_task;
    long jobs_ready_micro;
    long vis_zero;

    /* How many members are unframed: the denominator `jobs_ready` needs, taken on this walk so the pair is one
     * sample. Over members, as `vis_zero` beside `jobs_framed`: the question is whether anyone stands in the
     * state that could take a job. The scan asserts `<= members` and that `jobs_ready > 0` implies a member with
     * no frame. The converse is not asserted: `mem_unframed > 0` with `jobs_ready == 0` is the second silence. */
    long mem_unframed;

    /* How many dispatches the unframed population has received: raised in flow_credit_pick beside
     * `picks_lifetime` under the same flow_stack_empty the job and delivery splits use. A lifetime counter (a
     * decreasing series means it stopped being one), unlike the gauge `mem_unframed`. Read it beside
     * `picks_lifetime`, since 0 with no dispatches is silence:
     *   0 with dispatches made   the order never handed the thread to a member with an empty stack, which
     *                            includes every ready holder (the census asserts the ready arm implies
     *                            flow_stack_empty), so the defect is in the dispatch path, not the terms;
     *   above 0                  "ranked at the front and never taken" is refuted for the unframed population;
     *                            whether the dispatch compares `w_top` and the ready holder's weight stays open.
     * It is trustworthy while three checks hold: containment in `picks_lifetime` (asserted in flow_wfq_census and
     * re-asserted by engine/build.mjs), `picks_lifetime == _switches` across the document, and the raise and the
     * census arms both calling flow_stack_empty rather than restating it. A report, never a bound: no ordering
     * reads it, no fork carries it, nothing resets it. */
    int64_t unframed_picks_lifetime;

    /* How many of those unframed dispatches reached a member the ready arm would count: flow_credit_pick raises
     * it under `!flow_host_owed && flow_job_pending > 0` inside the `flow_stack_empty` arm, the job split's own
     * predicate. A lifetime counter (`jobs_ready` is the gauge), so it may be differenced. Read it beside
     * `unframedPicksLifetime` and `jobsRun`: `> 0` with `jobsRun` 0 means dispatches reach job holders and
     * flow_step declines the job at a higher arm (`jobsReadyTask`/`jobsReadyMicro` say which); 0 with
     * `picksLifetime` 0 is silence.
     * Its zero is not a verdict: flow_credit_pick's one caller is engine.c's `best != cur` block, so this counts
     * displacements, and an incumbent the pick retains on a tie is stepped without being credited. Read
     * `unframedStepsLifetime` and `stepUnitRuns`' `run-a-task` first. A positive count is a real refutation.
     * Named residual. Not covered: retained-incumbent steps. Next diff builds: the raise at the step entry
     * rather than at the switch credit. Absence shows as: a zero here beside a ready holder that held the
     * thread. Contained in `unframed_picks_lifetime` (asserted in flow_wfq_census, dev-only; engine/build.mjs
     * re-asserts the outer containment for release). A report, never a bound. */
    int64_t ready_picks_lifetime;

    /* The delivery backlog, split like the job backlog: where members holding an answered, untaken reply stand
     * in the order, which the cold census (`pendReady`, `canDeliver`) cannot say. Over members, because
     * pending_ready stops at the first deliverable entry while a full count is a walk cold_census already pays.
     * Disjoint and exhaustive over holders:
     *   `deliv_owed`   host-owed mark, so flow_pick refuses it. Zero when nothing is outstanding, since the mark's
     *                  assert admits only an entry the host can still be asked about (`pending_host_outstanding`)
     *                  or a referenced document; a declined entry is outstanding and owed by nobody.
     *   `deliv_framed` fails flow_stack_empty, and engine.c's task ladder is under `if (!f->frame)`, so its steps
     *                  never reach the delivery arm. Benign only while frames end (see `jobs_framed`).
     *   `deliv_ready`  neither: the reply waits on rank alone; the whole population an ordering could serve.
     * `canDeliver - delivReady` is the members the arm could serve and the pick will not offer the thread to
     * (`canDeliver` is `flow_stack_empty && pending_ready`, without the host-owed mark). */
    /* `deliv_w_gap` is `w_top` minus the best weight a ready holder offers, in the order's own points; 0 means the
     * front of the queue itself holds an undelivered reply. One completed unit drops the optimism bonus from
     * 1/(1+v) to 1/(2+v), half a point at v=0, while aging moves FLOW_AGE_QUANTUM per quantum, so a gap near 0.5
     * is one completed unit behind the front. Read it beside `deliv_ready` (both 0 with no ready holder). `>= 0`
     * by construction, as for `job_w_gap`, and asserted beside it. */
    long deliv_ready;
    long deliv_framed;
    long deliv_owed;
    double deliv_w_gap;
    /* The `visits` of the two members `deliv_w_gap` is between. Every other weight term has a row to price a gap
     * against (`val_top`, `top_svc`/`top_svc_fam`, `dist_max`), but the optimism step shrinks with the count, so
     * one gap is a different number of turns depending on where on 1/(1+v) the two stand. `visits` is the only
     * term a member raises only by finishing a turn, and a fork inherits its parent's count, so these say
     * whether the gap was earned. Gauges, read as a pair only where `deliv_ready` is non-zero (else both 0).
     * int64_t to match `Flow::visits`, since `long` is 32 bits in wasm. */
    int64_t deliv_w_gap_vis;  /* gauge: `visits` of the best ready holder — the member `deliv_w_gap` is from */
    int64_t w_top_vis;        /* gauge: `visits` of the member at `w_top` — the member `deliv_w_gap` is to */

    /* How far through the document's program table the deepest member has got, and what the order offers it.
     * `programCursors` (solver/cold.h) says where members stand and the rows above say what the order offers;
     * these join them. A gap of 0.0 means a member at the deepest row is at the front, so the members are
     * offered the thread in sequence and a stuck tail wants dispatches; a positive gap means the order ranks
     * shallower members ahead, which a term must fix. Same points as `never_picked_gap`.
     * Collected from weights the walk already computed, so `scanCensusWeights` is unchanged. Gauges: both may
     * fall, and `deepest`/`deepest_left` (solver/engine.h) are the monotone pair. `cur_deep` and
     * `cur_deep_live` repeat the top bucket of `programCursors`, a different walk; on one `workDone` they must
     * agree (a reading, not an assert, since nothing guarantees one instant).
     * A report, never a bound; a cursor term in the order would rank the youngest arm highest. */
    int  cur_deep;        /* gauge: the deepest `Flow.script_i` any live member stands at; 0 on an empty walk */
    long cur_deep_live;   /* gauge: how many live members stand there — the gap's own population */
    double cur_deep_w_gap; /* `w_top` minus the best weight offered by a member standing at `cur_deep` */
} WfqCensus;
void flow_wfq_census(WfqCensus *out);

/* What the order costs to ask. Each scan is linear in the frontier (flow_weight itself is O(1), since the preempt
 * hook reads it per opcode), so "the tail is not reached" has two causes: too little thread for the members, or
 * the thread spent asking the order. Entries are counted apart because their cadences differ:
 *   `next-to-run`: the dispatch loop's pick, one per step, so weights over steps is the frontier a step pays for;
 *   `rival-of-incumbent`: the preempt hook's rescan, one per miss of a key that is a disjunction
 *      (`flow_frontier_gen() != g_seen_gen || cur != g_seen_cur`), so a forking page pays per rank change and a
 *      dispatching one per switch; `rivalMissGen`/`rivalMissCur`/`rivalMissBoth` (solver/engine.h) partition it;
 *   `best-and-eviction-tail`: the host's best-weight read and the pager's tail, per report and at the RAM floor;
 *   `wfq-census-walk`: the census's own walk, one per sample (two samplers in a smoke: the result composer and
 *      the native fixture's probe table), so the instrument's cost is visible: compare `scanCensusWeights`
 *      against `scanNextWeights`.
 * A count, not a clock, so loaded runs stay comparable. It counts weight evaluations the scan performed (its
 * seed's and its loop's), not loop trips (excluded and host-owed members are skipped unpriced) and not a
 * DCHECK's. It decides nothing. */
#define FLOW_SCANS(X)                                                                     \
    /* the dispatch loop's pick — one per step */                                         \
    X(NEXT,  "next-to-run")                                                               \
    /* the preempt hook's rival rescan — one per miss of the generation-or-incumbent key;     \
       raises inside one C call collapse into one miss (see the banner above) */                \
    X(RIVAL, "rival-of-incumbent")                                                        \
    /* the host's best-weight read and the pager's tail, per report and at the RAM floor */\
    X(OTHER, "best-and-eviction-tail")                                                     \
    /* the census's own walk, one per sample; flow_wfq_census also calls flow_best, which   \
       weighs every member again under OTHER, so a sample costs two frontier weighings */    \
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

/* Lifetime scan count and weights evaluated per entry, per instance, never reset; their quotient is the
   frontier a scan walked. The weight count can be zero on a non-empty frontier: runnable-only scans skip a
   host-owed member before pricing it, so a frontier all waiting on the host prices nobody (engine.c's stall at
   `if (!best) break`). No floor is asserted between them. */
long flow_scan_runs(FlowScan s);
int64_t flow_scan_weights(FlowScan s);   /* per member per scan — 64-bit for FlowKeyChecks' reason */

/* What flow_pick's member-key assertion did with the weights the scan rows count; disjoint from them, since
   FLOW_SCANS never counts a DCHECK's weighing. The invariant is a predicted absence, and its condition is a
   four-way disjunction whose first three arms exempt the member, so a clean run cannot tell an invariant that
   holds from one never compared:
     - `armed`: comparisons actually made, the only row that scores the prediction;
     - `stale_gen`: the frontier generation moved since the member was weighed; near the total, the invariant
       is vacuous rather than held;
     - `first_seen` (at most one per member ever) and `running` (at most one per scan).
   One struct and one call, because the four are a partition and a partition is read at one instant. Lifetime
   counts of comparisons, monotone, may be differenced, never read against `members`. Zero in a release build,
   where the check is compiled out (see `Flow.key_last`) while solver/result.c still publishes them: all four
   zero with any `scan<Entry>Weights` nonzero is a build that makes no check, and `armed: 0` with the four
   summing above zero is the walk comparing nothing. flow_pick asserts their delta over one loop equals
   `g_scan_weights[why]`'s, which catches a `continue` inserted between the weighing and the block. They
   decide nothing. */
/* Counters raised once per member per scan grow as O(members × scans) and reach 2^31 within the hour on a real
   page, and `long` is 32 bits on wasm32 (the shipped host) while 64 on native, so a `long` here is a step cap
   only the vehicle has. They are int64_t, and the static assertion turns a narrower type into a compile error. */
typedef struct {
    int64_t armed;       /* comparisons the check actually made — the row that scores its predicted absence */
    int64_t stale_gen;   /* exempt: the frontier generation moved since this member was last weighed */
    int64_t first_seen;  /* exempt: this member had never been weighed, so there was nothing to compare against */
    int64_t running;     /* exempt: it held the thread, which is the one writer that may move its own half */
} FlowKeyChecks;
_Static_assert(sizeof(((FlowKeyChecks *)0)->armed) >= 8,
               "a per-member-per-scan lifetime counter is narrower than 64 bits — on the host where that is "
               "true it is a STEP CAP, which §NO BOUNDS forbids, and it fires as a wrapped comparison in "
               "solver/result.c rather than as anything a reader would connect to this declaration");
FlowKeyChecks flow_key_checks(void);

/* Whether an index over the member key would return the member the comparator did: the key standing still
   (above) is not the key ordering. In exact arithmetic the surrogate is the order, but flow_nonreward evaluates
   `((qn + opt) + dist) + bb` with the varying term first, so in floating point a surrogate is a re-association
   that can differ in the last bit. flow_pick therefore folds the surrogate over its own population and
   tie-break and counts what the two spellings did, with no equality assert, since flow_index_margin declares
   them free to stand that far apart. Its band walk (a candidate set within flow_index_margin of the
   surrogate's extremum, re-compared through flow_weight) asserts it returns the comparator's own extremum.
   `index_asked` is the reachability witness for all six rows. `differed` (named a different member) is
   partitioned by `differed_tie` (the surrogate reads the comparator's member at its own extremum: a lost
   distinction a candidate set answers) and `differed_strict` (a real disagreement). Read the strict share of
   `differed`, never a numerator alone.
   `band_members / band_weighed` is the share of the frontier a re-compare costs. The band always contains every
   member tied with the maximum (`S(m*) - S(a) <= 2M`), so its share is at least the tied share whatever the
   margin: a large share is the order being tied, not a loose margin. Lifetime counts under APICLIENT_DEV,
   deciding nothing. */
/* Named residual. Not covered: whether to re-compose flow_weight so its member half is a subexpression, which
   changes tie identity in the last bit and is the project owner's decision. Next diff builds: that decision,
   informed by `differed_tie`/`differed_strict` over whole runs. Absence shows as: a strict share that stays
   non-zero with the surrogate kept as only a re-association. */
typedef struct {
    long index_asked;      /* scans that folded the surrogate and had a maximum to compare it against */
    long index_differed;   /* …of those, the ones where the surrogate named a different member. It tests no
                              weight: the pair may weigh the same or differ in the last bits, and the two rows
                              below separate those */
    long differed_tie;     /* …of those, the ones where the surrogate reads the comparator's own member at its
                              own extremum: it lost a distinction the comparator makes rather than making a
                              different one, which is the arm a margin-carrying candidate set answers with
                              flow_weight untouched */
    long differed_strict;  /* …and the ones where it reads that member on the losing side of its extremum: a
                              real disagreement, which is the arm the tie-identity decision is about. The pair
                              is read against `index_differed`, which it partitions, and flow_pick asserts the
                              sum because the three are raised by two statements over one condition */
    /* The four `long` rows above are raised once per ask (outside flow_pick's member loop); the two below are
       raised inside the band walk, once per member per ask, so they are int64_t for FlowKeyChecks' reason. */
    int64_t band_members;  /* members the derived-margin candidate set admitted, summed over those scans */
    int64_t band_weighed;  /* …and the members that set was tested over — the denominator of the row above;
                              per member per scan, so 64-bit for FlowKeyChecks' reason */
} FlowIndexChecks;
_Static_assert(sizeof(((FlowIndexChecks *)0)->band_members) >= 8
                   && sizeof(((FlowIndexChecks *)0)->band_weighed) >= 8,
               "a per-member-per-scan lifetime counter in FlowIndexChecks is narrower than 64 bits — the two "
               "band rows share FlowKeyChecks' unit and therefore its wall, and the comment above says which "
               "rows of this struct are which so the answer is read rather than guessed");
FlowIndexChecks flow_index_checks(void);

/* Lifetime count of rank changes (raises of frontier_rank_changed): the denominator of the hook's rescan count.
 * The hook rescans when `flow_frontier_gen() != g_seen_gen || cur != g_seen_cur` (engine.c), so its cadence is
 * set by rank changes and incumbent switches, not steps: rescans per step is a cost, rescans per rank change is
 * the cache's hit rate. Counted at the raise rather than read from flow_frontier_gen, because
 * flow_registry_init resets the generation and none of the scan counters. Decides nothing. */
long flow_rank_changes(void);
/* Lifetime arrival and departure counts for this instance; see the census rows for the reading and identity. */
int64_t flow_arrivals(void);
int64_t flow_departures(void);
/* The third arm of the departure total: members still standing when this instance's registry went down. The
   other arms (`finished`, `sold`) are engine.c's, so the partition is asserted in the frontier census, where a
   departure crediting no arm would show as a fourth exit. */
int64_t flow_departures_teardown(void);

/* Lifetime count of dispatch-loop picks that switched to a member already dispatched before while a
   never-dispatched member stood at exactly its weight. Read as a fraction of `picks_lifetime`: a handful is the
   strict comparison working; a share near the dispatches is the order no longer separating members, so registry
   position decides. It does not separate a framed flow re-picked to finish its program (the ordinary case on a
   forking page, since an arm is born at its parent's weight) from a pass over a starved member;
   flow_starved_picks_idle is that subset, and the remainder is what a forking frontier spends finishing
   programs. */
long flow_starved_picks(void);

/* Lifetime sum of the rebuild an epoch-keyed index over flow_index_key would pay (published as
   `epochRebuildLifetime`; flow.c holds the derivation). Its denominator is `scanNextWeights`, the walk an index
   would replace: well below it an index narrows, at or above it the walk is only moved. `epochResetsLifetime`
   gives the average rebuild per emission. Read a zero against the resets first: zero resets is a run that
   never emitted; zero rebuild over non-zero resets is the best result, reachable when nothing was charged
   since the last finding. Release-live: four O(1) maintenance statements. */
long flow_epoch_rebuild(void);

/* Lifetime count of emissions that reset a family's base (`epochResetsLifetime`), raised in the same statement
   group as the sum above, so the two describe one population. */
long flow_epoch_resets(void);

/* The subset of flow_starved_picks in which the re-dispatched member had nothing to continue: no frame, no
   parked continuation and no microtask owed, the unit boundary HTML §8.1.4.4 "Calling scripts" (clean up after
   running script step 3) draws and `visits` is credited at. Re-dispatching such a member past a level waiter
   is starvation with no necessity behind it. Raised under the superset's condition at one line, so
   `idle <= starved` by construction; a lifetime counter, read as a fraction of `picks_lifetime`.
   It asks all three clauses of engine.c's boundary (`!frame && !JS_HasParkedFlow(runtime) &&
   !flow_job_microtask`): flow_between_units asks the first and third, flow_holds_park the second. The raise
   requires `best != seed`, so the member counted is never the running one, whose park queue is in the
   runtime; flow_holds_park asserts that. */
long flow_starved_picks_idle(void);

/* How deep the tied plateau is: lifetime rows that separate the attention value yield (a top-ranked incumbent
   runs on at near-zero switch cost; a retention is that) from the never-starved guarantee (about a member
   ranked behind; a tied member is ranked behind nobody). Both can be wrong only about how long a retention
   lasts. The aging spec predicts a bound: flow_age_running's charges telescope across turns, so the
   incumbent's own silence outruns a level waiter's notch and it hands over (engine.c asserts that at the
   charge).
     `plateauHeld / plateauAsked`   how tied the frontier is at the dispatch line; not a defect by itself.
     `plateauHeld / plateauRuns`    the depth: bounded is the queue rotating; `plateauRuns` at 1 beside a large
                                    `plateauHeld` is one unbroken hold, the guarantee false.
   `plateauAsked` is the reachability witness; a zero ask beside non-zero `picksLifetime` is a frontier holding
   no never-dispatched member at any dispatch. Release-live, deciding nothing. `plateauHeldIdle` is an upper
   bound (two clauses of the unit boundary are not asked; its residual is at its counter in flow.c). */
/* Named residual. Not covered: engine/build.mjs's @WFQ verdict says nothing about plateau depth. Next diff
   builds: that verdict clause, with a band derived from the first measured depth. Absence shows as: a verdict
   naming a frontier advancing without retiring while `plateauRuns` is 1 and `plateauHeld` is in the thousands. */
long flow_plateau_asked(void);
long flow_plateau_held(void);
long flow_plateau_runs(void);
long flow_plateau_held_idle(void);

/* The highest-priority flow in the frontier, or NULL if empty: every member, whether or not it can currently
   make progress. It answers the host's Level-1 question (this document's best weight) and the census's; the
   scheduler's own pick is flow_next_to_run below. Does not remove it. */
Flow *flow_best(void);

/* Which flow should hold the thread, given the one that holds it now (NULL when nobody does): the dispatch
 * loop's pick. Same comparator and order as flow_best, plus two scheduling rules:
 *   - a host-owed flow is not a candidate (it cannot use the thread), and
 *   - the incumbent keeps the thread unless a candidate is strictly better, the same comparison the preempt
 *     hook's value clause makes, so a tie never swaps two COW deltas.
 * NULL means nothing can run: the frontier is empty or every member waits on the host (the stall, decided by
 * asking each member). `why` names the asker, because the dispatch loop (per step) and the host's Level-1 poll
 * (engine_top_weight) pass identical arguments at different cadences, and FLOW_SCANS counts them apart. */
Flow *flow_next_to_run(const Flow *incumbent, FlowScan why);

/* The best flow that could use the thread other than `cur`: what the preempt hook compares the running flow
 * against, hence it excludes rather than seeds. The same scan, so the hook and the pick never disagree. */
Flow *flow_rival_of(const Flow *cur);

/* Whole quanta of thread time this flow has consumed: the quantised own silence, a census quantity. Not itself
 * a weight term; the aging term reads its sum with the family notch in this unit. */
int64_t flow_service_notch(const Flow *f);
/* The family's notch, in the same unit; it has its own reset point (flow.c's flow_age_running). The ratio of the
   two is the widest family's fork factor. */
int64_t flow_family_notch(const Flow *f);
/* The aging term's quantity: whole cooperative quanta of silence, `(own + fam) / FLOW_SERVICE_US`. The price per
   notch is applied where the term is summed (flow.c's FLOW_AGE_QUANTUM), since the cost of a microsecond and the
   order's smallest step are different quantities. Public because it is half of a rank change: between two of
   these notches and two completed units, a weight moves only through an emission, which is the invariant
   engine.c's seam assertion holds the value yield to. */
int64_t flow_silence_notch(const Flow *f);

/* The remainder and carry of that division: `(own + fam) / S` is `own / S + fam / S` plus a carry of one notch,
   so the three notch rows do not reconcile without these. Between two frontier generations the carry is the
   only per-member quantity in the weight that moves, its threshold is common to the family and sweeps
   monotonically, so members cross in descending phase and a frontier maximum is a maximum over two contiguous
   phase ranges (flow.c states the derivation). They price an ask; no weight term reads them. */
int64_t flow_silence_phase(const Flow *f);
int flow_silence_carry(const Flow *f);

/* The member's top-level arm's `sub_born`, the denominator of flow_branch_bonus (`1.0 / sub_born`), published
   because FlowAcct is private to flow.c and one fork raises it for every member of the arm. engine.c's rival
   assertion reads it to check what may move while the generation stands still. The integer rather than the
   term, so equality is exact. Reconciles against `brBornMax`/`brBornMin`. Not a term; nothing ranks by it. */
long flow_branch_born(const Flow *f);

/* The lowest-priority member other than `exclude`: the tail the cold tier gives up first at the RAM floor, the
 * same comparator as flow_best read the other way, so the flow paged out is the one the WFQ would run last.
 * Asserted where computed by re-deriving the minimum over the same candidate set. Every member, not only the
 * runnable ones: a host-waiting flow is the cheapest to page (its recipe re-issues the request).
 * `exclude` is the flow the scheduler is switched into, which can be neither written out nor released. */
Flow *flow_worst(const Flow *exclude);

/* This flow answered FLOW_STEP_OWED. It stays in the frontier at its own weight with every work item; it is not
   picked again until the host does something that could answer it. Asserts the flow was not already marked,
   since a marked flow is out of the pick and a second report means its mark was cleared without an event. */
void  flow_set_host_owed(Flow *f);

/* How many members have reported themselves host-owed. Beside the census's `blocked` (which asks the register),
   a gap means marks are cleared faster than the sweep lays them; equal numbers on a stalled frontier is
   healthy. */
int   flow_host_owed_count(void);

/* Is this one member out of the runnable order (flow_pick's own filter)? False for a live member means the scan
   weighed it, which a caller comparing its incumbent against the scan's maximum needs; published so that the
   mark keeps one writer rather than being inferred from the step code. */
int   flow_host_owed(const Flow *f);

/* The host answered this flow, so it is askable again. One clear per event, on the flow the event reached: a
 * reply provided into its register, an answer delivered, a record or operation attached to it. Tied to the
 * event rather than to the slice, because a slice that ended on its CPU quantum gave the host nothing to answer,
 * and clearing per slice would make "every member waits on the host" unreachable. */
void  flow_clear_host_owed(Flow *f);

/* The one event that names no flow: an external document script's text is the document's, so its reply fills a
   slot every flow parked on that script was waiting for. The only unblocking inside a slice. */
void  flow_clear_host_owed_all(void);

/* The image this member owes the host (`paint_owed`). Marked on two roads, the call sites of
 * `engine_request_paint_every_world`: `paint_mark_standing_members` marks the members alive at the ask, and
 * `flow_new` marks members born later; either alone misses a population (the walk misses later arms, the mint
 * misses the boot flow). A mark is discharged when the scheduler hands the thread back with that member
 * switched in. It is not a host-owed mark: that takes a member out of the pick, while this changes the pick not
 * at all, and folding them would stall an asked frontier. */
void  flow_set_paint_owed(Flow *f);
void  flow_clear_paint_owed(Flow *f);
int   flow_paint_owed(const Flow *f);
/* Every world minted from now on owes the host one image: a standing mode deciding what a member is born with,
   unlike flow_set_paint_owed. One-way, so a run's world-named images are a complete set of its worlds rather
   than a sample. Members alive at the ask are `engine_request_paint_every_world`'s walk. */
void  flow_paint_every_world(void);

/* A counter bumped at every rank change (frontier_rank_changed: membership changes, emissions, fitness
   observations, a completed unit, host-owed transitions). The value yield recomputes its rival only when this
   changes or the running flow switches, never per opcode; flow_age_running's charge deliberately does not bump
   it. */
unsigned flow_frontier_gen(void);

/* The running flow (scheduler-set). Detectors credit emitted value to it; the scheduler ages it. */
void  flow_set_running(Flow *f);
Flow *flow_running(void);
void  flow_credit_emit(double v);   /* a new @H/@S from the running flow: raise reward, reset aging */
/* Where this candidate's own bytes stand on the @S ladder: the fitness term of flow_weight, composed from the
   rung fields rather than stored, `(cand_replay + cand_surv + cand_rung) / FLOW_RUNGS_N` in [0,1], and exactly
   0 for a flow that is not a candidate (both fitness writers assert they are asked only of one). */
double flow_distance(const Flow *f);
/* A candidate has replayed part of its own recorded path: rung zero. `consumed` is the arms this run has
 * replayed and `total` the recorded path they prefix; the reading is their ratio, written, never accumulated.
 * Called from decide.c's dec_replay one arm at a time, strictly before the source read. */
void  flow_observe_replay(Flow *f, long consumed, long total);

/* The running candidate's own bytes survived this much of the page: rung one. `frac`, in [0,1], is the fraction
   of this flow's payload a re-execution delivered. It raises `cand_surv` and does nothing when the flow already
   stands further along, so it never demotes the flow and may be repeated. It bumps the frontier generation, as
   an emission does, because the weight moved. Asserts the delivery rung, since a fraction is measured on this
   flow's payload and a flow whose payload is not in the program has nothing to measure. */
void  flow_observe_survival(Flow *f, double frac);
/* The rung this flow's bytes have now reached: FLOW_RUNG_DELIVERED, _ARRIVED or _ESCAPED. A separate entry
   because the observations are made at separate sites that cannot see each other's answers: the delivery where
   the substitution is performed (solver/concolic.c, a source read), the survival fraction at every
   code-execution sink, and the two sink rungs inside the candidate's own class by that class's language.
   Monotone and ordered: it raises `cand_rung`, never lowers it, and refuses a rung whose predecessor this flow
   has not stood on. */
void  flow_observe_rung(Flow *f, int rung);
/* The running flow just took an arm its concrete example contradicts: `path_forced`'s one writer. Idempotent and
   monotone. Not a credit or a charge: a fork is rank-neutral, so it does not bump the frontier generation. */
void  flow_mark_forced_arm(void);
/* Has this flow's path stood on one? Read at the park, because a parked request is a work item that takes its
   provenance with it: it was built on the path the flow had then. */
int   flow_path_forced(const Flow *f);
/* The running flow just determined a source's value on an arm its own example contradicts: `path_pinned`'s one
   writer, idempotent and monotone. Called only where decide.c's decide_note_forced_arm has just answered that it
   marked the path, so the nesting is structural and "did this arm contradict its example" has one spelling. */
void  flow_mark_pinned_value(void);
/* Has this flow determined a value on such an arm? Read at the park, beside flow_path_forced and for its reason. */
int   flow_path_pinned(const Flow *f);
/* Charge the running flow for the thread time a step just burned, in microseconds (the reward's currency), after
   the step and by the scheduler alone, which holds both ends of the interval. Never a step count (see `cpu`). A
   flow that leaves the frontier also hands what it burned to the flow that forked it, which prices a fork
   chain. */
void  flow_age_running(int64_t us);

/* The forked sibling takes over its parent's account: both rank terms at the instant of the branch, and the fork
   edge that sends the thread time the arm burns to its family. Without it an arm would read zero reward and
   the full never-run bonus and outrank every flow that ever had a turn. Called by the fork only; a
   from-baseline flow (the first flow, a candidate session, a cold resume) keeps the constructor's zeros. Asserts
   the fork is rank-neutral (reasoning at the definition). */
void  flow_fork_inherit(Flow *sib, const Flow *parent);

/* Release a flow the scheduler is not switched into: the one teardown for a frontier member, and the primitive
 * the partial self-park to the cold tier needs. It removes the flow and returns its RAM: its suspended frames,
 * heap COW delta, DOM head and reference on the frozen chain, decision and pin blobs, chunk bodies, queued jobs
 * and owed replies. Everything is released as parked state: the delta's head is freed rather than unapplied,
 * and only what the release frees is walked out of the live heap and document (cow_delta_release /
 * dom_base_release), so the flow holding the thread is undisturbed. Switch the flow out first; both halves
 * assert it. It is the only teardown, so a field added to `Flow` has one place to learn about it, and
 * flow_remove asserts it did. */
void  flow_release(JSContext *ctx, Flow *f);

/* Remove + free a flow whose state has already been released. Asserts what flow_release owes it. */
void  flow_remove(JSContext *ctx, Flow *f);

int   flow_count(void);
/* Is this pointer still a member of the frontier? Pure, so a DCHECK may ask it. A Flow* is held across a return
   to the host (engine.c's g_sess_cur) and across a switch-out, and a removed flow is freed. */
int   flow_is_member(const Flow *f);
/* The i'th flow in registry order, or NULL past the end — a walk over the frontier's members, which is what a
   register living on the flows needs. flow_best answers which one to run; this answers who exists. */
Flow *flow_at(int i);

/* How many queued programs the whole frontier holds for one document, summed over every member's `dyn_doc`.
 * A `dyn_doc` entry is a uint32 handle that keeps nothing alive and stays readable after its realm is gone,
 * unlike every counted JSValue by which a flow names a realm, so core/frame/navigable.c asserts this at the
 * moment a realm dies. Pure (no allocation, no JS value, no reference), because the realm-teardown hook runs
 * inside a collection. */
int   flow_programs_for_document(uint32_t doc);

/* How many programs of one document one flow has queued and not started. Per flow because HTML §7.5.10
 * "Destroying documents" runs once per timeline over its own delta, and another flow's rows for that document
 * belong to a document that still exists there. Unstarted because step 5 removes tasks "without running those
 * tasks"; a compiled row may hold a suspended continuation, which is the flow's own state. `last_compiled` is
 * the line between them. Pure, so a DCHECK may ask it. */
int   flow_programs_unstarted_for_document(const Flow *f, uint32_t doc);

/* HTML §7.5.10 "Destroying documents" step 5, "Remove any tasks whose document is document from any task queue
 * (without running those tasks)", on the queue JS_DropJobsForContext cannot see: a document's scripts are rows
 * of the running flow's program sequence, and a row is a task (§8.1.4.4 "Calling scripts" runs it; §4.12.1.1
 * "Processing model" queues it). Returns how many rows went, which the count above answered just before.
 * The running flow's rows only, since another timeline has not destroyed the document. It repairs `imm_at` and
 * `imm_next`, which are absolute; `script_i` and `last_compiled` are unaffected (see the note in the body). The
 * pending register needs no repair, since entries name rows by `dyn_id`; the walk drops the entries whose rows
 * go (HTML §7.5.11 "Aborting a document load" step 2's "Cancel any instances of the fetch algorithm in the
 * context of document"). Not pure: it frees rows and mutates the register, so never call it inside a
 * collection or a DCHECK. */
int   flow_programs_remove_for_document(Flow *f, uint32_t doc);

/* Which row of `f`'s sequence is called `id`, or -1 when this flow holds none (see `dyn_id`). Pure, so it may
 * stand in a DCHECK, and it asserts nothing itself: each of its three callers (the two halves of a document
 * script's delivery and §7.5.10's removal walk) says what its own -1 means. */
int   flow_dyn_row_by_id(const Flow *f, uint64_t id);

#endif
