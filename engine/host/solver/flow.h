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
    signed char *dec;      /* the DECISION VECTOR — the arm (0/1) this flow takes at each branch, in order */
    int dec_n;             /* length of dec */
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

    int   started;         /* decide_enter has run (fresh) — else resume from the blobs below */
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
} Flow;

/* `doc_name` is THIS INSTANCE'S DOCUMENT identity, and it is a parameter rather than a separate init call so a
   frontier cannot exist without one: every flow is minted a world named by it, and two instances that shared a
   name would hand each other's flows the same segment. The host names the ROOT document, because the host is
   what knows there is more than one; every document below it is named by the one that created it. */
void  flow_registry_init(const char *doc_name);
void  flow_registry_free(JSContext *ctx);

/* Add a flow to the frontier. Takes ownership of `dec` (freed with the flow) and dups `fn`. `dec` may be NULL
   (dec_n 0) for a from-baseline flow. Returns the stored Flow* (stable until removed). Never fails (OOM aborts
   via CHECK — a dropped flow corrupts the frontier). */
Flow *flow_add(JSContext *ctx, JSValueConst fn, signed char *dec, int dec_n, WorldId parent);
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

/* The highest-priority flow in the frontier, or NULL if empty. Does not remove it. */
Flow *flow_best(void);

/* The highest-priority flow OTHER than `exclude` — the running flow's rival for the value-driven yield. */
Flow *flow_best_other(const Flow *exclude);

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

/* Remove + free a flow (its emitted work is done, or it was evicted). */
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
