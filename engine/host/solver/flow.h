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
 * emitting sinks below productive+unrun flows). ORDER-only: it never drops a work item. */
#ifndef ENGINE_HOST_SOLVER_FLOW_H
#define ENGINE_HOST_SOLVER_FLOW_H

#include "quickjs.h"
#include "solver/world.h"
#include "solver/pending.h"   /* the replies the host still owes this flow — a JS Array, not a malloc'd list */

/* One queued microtask/reaction job owned by a flow (routed here by the job-enqueue hook, not a global list):
   the quickjs job function + its dup'd arguments, run under the flow's COW after its scripts. */
/* A queued job of this flow's, and WHICH of HTML 8.1.7's two queues it came from. `task` is not a label: the
   event loop performs a microtask checkpoint between one task and the next, so a task may not run while this
   flow still holds a microtask. One array keeps the two in a single arrival order — which is what a task source
   needs among its own tasks — and the pick applies the checkpoint rule. */
/* `ctx` IS THE REALM THAT ENQUEUED IT — a document is one realm, and HTML §7.5.10 step 7 removes every task
   whose document is a destroyed one WITHOUT running it. quickjs records the same field on the entries it keeps
   itself; a job the host TOOK has to carry it too, or a destroyed document's reactions stay queued and run
   against a Document whose browsing context is null. BORROWED: the agent owns its realms. */
typedef struct { JSContext *ctx; JSJobFunc *fn; int argc; JSValue *argv; int task; } FlowJob;

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
       its targets by live heap pointers), so the name is what crosses — see solver/world.h. */
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
    /* THE AGING TERM, AND ITS UNIT IS THE WHOLE OF WHETHER THE TERM WORKS. Thread time in MICROSECONDS burned
       since this flow's last emit — never a step/opcode/visit count. A count is not commensurate with `val`
       above: a step used to be a whole drain and became one unit of work, so the same charge billed a flow the
       same amount for twelve milliseconds of execution as for advancing a script index, and §scheduler's
       sentence ("a monopolizer that burns CPU without emitting sinks below productive AND unrun flows") could
       not be true at any rate expressible in steps. In microseconds the two terms share a currency and the
       exchange is stated once, at FLOW_AGE_RATE.
       int64 rather than long because `long` is 32 bits in wasm: 2147 seconds of unproductive CPU would overflow
       it, and a NEGATIVE cpu makes the monopolizer the highest-ranked flow in the frontier — the exact failure
       this term exists to prevent, arriving silently after 36 minutes. */
    int64_t cpu;

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
    int cand_fired;        /* this flow's X9 marker executed */
    int cand_verifying;    /* this flow is a candidate run: the sink takes the concrete arg */

    /* AN ENGINE-SEEDED DISCOVERY PROBE — the ONE candidate document address this flow exists to read
       (solver/discovery.h). It is not a different KIND of flow either: it is ranked, preempted, parked and
       resumed by the same WFQ as the boot fork and an @S candidate session, and its unit of work is the park
       every other flow already uses. What it does NOT have is a page program — a probe runs no script, so
       flow_step answers it before the document's script sequence and never enters one.
       IT IS ALSO THE FLOW'S WHOLE IDENTITY, which is what makes the cold tier's record of it one string
       (solver/cold.h's `d`): a resumed probe re-issues the GET and reads TODAY's document, which is exactly
       what §Time-travel-resume asks of a flow whose work IS a fetch. Owned; NULL on every other flow. */
    char *disc_url;

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
    /* THE DOCUMENT'S LOAD STAGE IS NOT HERE, and the field that was is DELETED. One integer cannot hold N
       documents: an agent is an origin-keyed CLUSTER, so a flow reaches several Documents and HTML gives each
       its own readiness and its own DOMContentLoaded. The stage lives on each Document (document.c's readiness
       slot), which is a heap write the COW delta already isolates per flow — so it is still per-flow, and it is
       now also per-document, which is what it always had to be. */
    int   script_i;        /* position in the script sequence: static [0,n), then this flow's dyn chunks [n, n+dyn_n) */
    /* THE HIGHEST SCRIPT INDEX THIS FLOW HAS COMPILED, so that compiling one twice can be caught. A flow runs
       each program in its sequence ONCE; a preempted flow RESUMES its suspended frame and never re-enters
       JS_FlowNew for a program it already started. Re-compiling is a REPLAY, which this engine does not do — a
       replay re-executes side effects the flow already performed against a delta that already holds them. */
    int   last_compiled;   /* -1 until the flow compiles its first program */
    char **dyn; int dyn_n, dyn_cap;   /* this flow's OWN lazily-loaded chunk bodies (per-flow, not global) */
    /* WHAT KIND each of those programs is (a DynKind, engine.c). A page script that does not compile is a real
       problem and asserts; the two other kinds are ORDINARY when they do not. An @S CANDIDATE that does not
       compile is the common case — most breakouts do not fit most sink contexts, which is why the solver tries
       several and keeps the one that FIRES, and CLAUDE.md names it: an unsolved @S candidate is a parked search,
       never a @WHY. A `javascript:` URL that does not compile is HTML §7.4.2.3.2's abrupt evaluation, which
       simply produces no Document. Kept as a parallel array so the page-script assert stays fully armed inside a
       candidate flow, which still loads real chunks. */
    unsigned char *dyn_cand;
    void *delta;           /* this flow's isolated HEAP COW delta (CowDelta*), applied while running */
    void *dom; int dom_n, dom_cap;   /* this flow's isolated DOM COW delta HEAD buffer (dom_cow), swapped with the
                                        heap delta on every context-switch so the DOCUMENT is a per-flow time-travel
                                        entity: two flows see different trees/attributes, a rewind restores the
                                        exact document the flow saw. Detached via dom_buf_take while parked. */
    void *dom_base;        /* the shared IMMUTABLE base-segment chain below the head (dom_cow_fork): a snapshot-
                              forked sibling references the parent's O(N) DOM delta in O(1). NULL until a fork. */
    void *dec_blob;        /* suspended decision state while paused (decide_suspend) */
    void *pin_blob;        /* suspended pin state while paused (concolic_pins_suspend) */
    FlowJob *jobs; int njob, jobcap;   /* ASYNC-AS-FLOW: this flow's OWN queued microtasks AND tasks, drained
                                          after its scripts under its live COW (correct ordering, per-flow isolated) */
    /* FETCH-AWAIT: this flow's OWN live (pending) fetches and the synchronous requests it is blocked on,
       resolved when the flow's scripts+microtasks stall (the network completing). A JS ARRAY of plain records
       (solver/pending.h) rather than a malloc'd list, because CLAUDE.md §State-isolation says so in as many
       words: it must park to the cold tier, resume byte-identically and fork per-flow, and a `char *` does
       none of the three. JS_UNDEFINED for a flow that has never parked on anything, which is most of them. */
    JSValue pending;
    /* THE PARKED CONTINUATION, swapped with everything else on a context switch. A forced preempt inside
       job-driven code parks an async activation in the RUNTIME's one slot; that activation belongs to THIS
       flow's timeline and resumes under THIS flow's delta, so leaving it in the runtime while a sibling runs
       would either resume it against the wrong heap or drop it outright. Empty (park_fn NULL) for a flow with
       nothing parked, which is every flow that has not preempted inside a reaction. */
    void *park_ctx; void *park_fn; void *park_opaque;
    /* A ROUTED CROSS-DOCUMENT DELIVERY — the record the trusted zone handed this instance, and the SENDER's
       origin, which only that zone may stamp (SECURITY.md: an origin the untrusted engine computed for a
       foreign message is a forgery every `event.origin` check in every bundle would then trust). A delivery is
       a WORK ITEM ON THE ONE FRONTIER and this is how it is carried. It is attached to EVERY live flow of the
       receiving document, because a document's state IS its flows: the page's `message` listener was registered
       by a script, so it lives in the delta of the flow that ran it, and a delivery made anywhere else arrives
       at a document where nothing is listening. The flow's next step consumes the record, and the task that
       step enqueues lands on that flow's own queue like any other job — which is why the queue is per-flow.
       Both owned; NULL on a flow with nothing to deliver, and NULL again the moment the delivery is made. */
    char *deliver;
    char *deliver_origin;
    /* A CROSS-AGENT OPERATION THIS INSTANCE WAS ASKED TO PERFORM — the record the asking instance wrote
       (core/frame/remote_op.h) and the trusted zone's rendezvous TOKEN for the flow that is waiting on it.
       IT IS THE SAME SHAPE AS THE DELIVERY ABOVE AND FOR THE SAME REASON, with one thing added: a delivery is
       one-way and this one owes an ANSWER. A document's state IS its flows, so `otherW.length` has N answers
       for N timelines — the record is attached to every live flow exactly as a delivery is, and each of them
       answers under its own delta. A channel that carried one answer would silently pick a timeline.
       `perform` is consumed when the flow QUEUES the operation's program; `answer_token` outlives it, because
       the answer is the program's COMPLETION and that does not exist until the program ends. A fork inside that
       program is a peer timeline that also answers, so the sibling inherits the token rather than dropping it.
       Both owned; NULL on a flow with no operation outstanding. */
    char *perform;
    char *answer_token;
    /* AND WHICH DOCUMENT OF THIS AGENT THE OPERATION NAMES, which is WHERE its program runs. An instance is an
       origin-keyed agent CLUSTER, so the document a peer asks about is a child navigable as often as it is the
       root — and §7.2.5.1's member is read of THAT document's Window, so a program compiled in the root's
       realm would count the root's child navigables and hand them back as the child's. It is held beside the
       token and cleared with it, for the same reason the token outlives `perform`: the program is compiled
       after the record is consumed, and the realm has to be selected then.
       A DOCUMENT HANDLE AND NOT A JSContext*, because a handle is what crossed the wire, what a park can carry
       and what a resumed flow can still resolve — a realm is none of the three. 0 on a flow with no operation
       outstanding, which is every flow that has never been asked one. */
    uint32_t perform_doc;
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
   Never fails (OOM aborts via CHECK — a dropped flow corrupts the frontier). */
Flow *flow_add(JSContext *ctx, JSValueConst fn, WorldId parent);
/* How many flows this document ever created — the other half of the switch count. A run whose cost jumped needs
   to say WHICH grew: the frontier, or the work per flow. */
long flow_created_count(void);

/* IS THIS FLOW BLOCKED ON THE HOST? True while it holds an unanswered synchronous request. A blocked flow
   cannot make progress, so the preempt hook always yields it and a mid-frame yield reports it host-owed rather
   than runnable — otherwise the scheduler re-enters it immediately and it spins on an answer that cannot
   arrive while it holds the thread. */
int flow_blocked(const Flow *f);

/* The WFQ priority of a flow (higher = run sooner). Pure function of the flow's reward/aging/visit state. */
double flow_weight(const Flow *f);

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

/* HOW MANY WHOLE QUANTA OF THREAD TIME THIS FLOW HAS CONSUMED — the quantised reading of `cpu` that BOTH terms
 * of flow_weight are built from, exposed because it is what a rank CHANGE is made of. Between two notches a
 * flow's own weight cannot move except through an emission, and that pair of facts is exactly the invariant
 * engine.c's seam assertion holds the value yield to. */
int64_t flow_service_notch(const Flow *f);

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
   that drains that reply fills a slot every other flow parked on the same script index was waiting for. It is
   the only unblocking that happens inside a slice, which is why it is the only clear that is not per flow. */
void  flow_clear_host_owed_all(void);

/* A counter bumped on every frontier membership change (add/remove). The value-yield recomputes its rival
   only when this changes (or the running flow switches), never per-opcode. */
unsigned flow_frontier_gen(void);

/* The running flow (scheduler-set). Detectors credit emitted value to it; the scheduler ages it. */
void  flow_set_running(Flow *f);
Flow *flow_running(void);
void  flow_credit_emit(double v);   /* a NEW @H/@S from the running flow: raise reward, reset aging */
/* CHARGE THE RUNNING FLOW FOR THE THREAD TIME A STEP JUST BURNED, in MICROSECONDS — the same currency as the
   reward above, which is the only reason the aging term can ever outweigh it. Charged AFTER the step, because
   the quantity is not known before it, and by the scheduler alone (it is the only caller that holds both ends
   of the interval). Never a step count: see the `cpu` field. */
void  flow_age_running(int64_t us);

/* THE FORKED SIBLING TAKES OVER ITS PARENT'S ACCOUNT — both terms of the rank, at the instant of the branch.
   A fork copies every other field of the parent's history (frame, delta, DOM base, jobs, replies, chunks) and
   used to leave these two at the constructor's zeros, which told the WFQ that a flow running since boot had
   emitted nothing and consumed no thread — the second of those being the FULL never-run optimism bonus. The
   sibling then outranked every flow that had ever had a turn, so the frontier could only be entered and never
   drained. Called by the fork and by nothing else; a from-baseline flow (the first flow, a candidate session,
   a cold resume) keeps the zeros, which is what makes ITS bonus mean what it says. Asserts that a fork is
   RANK-NEUTRAL — see the reasoning at the definition. */
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
 * `park_fn` claim the teardown did not make, and the teardown freed a delta the finish path had already
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

#endif
