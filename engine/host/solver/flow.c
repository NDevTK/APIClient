/* Flow registry + WFQ — see flow.h.
 *
 * The registry also owns the teardown of a flow that NEVER FINISHED, which is why this file knows what a
 * flow's state is made of: the fields are declared in flow.h, so their destructors belong wherever a Flow is
 * freed. The alternative — a scheduler that must remember to sweep the survivors before the registry goes down
 * — is the shape where a host that forgets retains the whole runtime with nothing to name the owner. */
#include "solver/flow.h"
#include "solver/engine.h"     /* ENGINE_QUANTUM_MS — the scheduler's slice IS this file's service quantum */
#include "solver/cow.h"        /* a survivor's heap COW delta */
#include "solver/dom_cow.h"    /* …and its DOM delta head + shared base segment */
#include "solver/decide.h"     /* …and its suspended decision vector */
#include "solver/concolic.h"   /* …and its suspended path constraint */
#include "solver/cold.h"       /* …and the park document written out of all of them */
#include "solver/reclaim.h"   /* the engine's own allocations ask for a flow back before they fail */
#include "check.h"
#include <stdlib.h>

static Flow **g_flows = NULL;
static int g_flows_n = 0, g_flows_cap = 0;
static unsigned g_gen = 0;   /* bumped whenever the frontier's membership changes (add/remove) — lets the
                                value-yield recompute the rival only on a change, never per-opcode */
static Flow *g_running = NULL;   /* the flow currently holding the worker (the scheduler sets it) */

/* THE FORK TREE, which is what the aging term is actually about — and the one thing in this file that is not a
 * property of a single flow.
 *
 * §scheduler prices the aging term so that "a monopolizer that burns CPU without emitting sinks below
 * productive+unrun flows". A FORK CHAIN IS ONE MONOPOLIZER WEARING N NAMES, and that is not a hypothetical
 * shape: quickjs.c's step_length_unknown walk forks a "stop at n" arm at EVERY position, each arm runs the rest
 * of the document and then FINISHES, and `cpu` charged the ARM. So the thread time the walk really spent went to
 * flows that ceased to exist; the walker's own recorded service grew only by the per-position ask, so the family
 * never sank. Measured on the minimal fixture: 22 of 26 probe rows stayed 0 and were FLAT from sample 16 of 113,
 * every one of them waiting on a sibling arm the walk family outranked for the whole run. quickjs.c's claim that
 * "CPU-aging sinks it below every flow that does emit" was true of the TERM and false of what the term could SEE.
 *
 * SO A DEPARTING FLOW HANDS ITS SERVICE TO THE FLOW THAT FORKED IT. That is the whole mechanism, and it is
 * hierarchical fair-share's own rule rather than a new policy: a group is charged for everything below it, or any
 * member sheds its debt by splitting. flow_weight is UNCHANGED; what changed is what `cpu` accumulates.
 *
 * WHY A NODE AND NOT A `Flow *` PARENT. An arm can outlive the flow that forked it, so a pointer at the parent
 * is a pointer at freed memory exactly when the charge needs it. This node is refcounted by its owning flow AND
 * by every live child's `up`, so the ancestry stays addressable for precisely as long as something below it can
 * still be charged and no longer. `owner == NULL` is that flow having left the frontier: a charge walks PAST it
 * to the nearest LIVE ancestor, because a departed flow has no rank left for the service to move, and it
 * COMPRESSES the run it walked — union-find's find, which is why a deep dead ancestry is paid for once however
 * many flows depart below it. */
typedef struct FlowAcct {
    Flow *owner;              /* the flow this node accounts for; NULL once it has left the frontier */
    struct FlowAcct *up;      /* the node of the flow this one was FORKED from; NULL for a from-baseline flow */
    int refcount;             /* the owner's reference + every live child's `up` */
} FlowAcct;

/* THE CENSUS, counted at the two points a node's lifetime begins and ends so the pair cannot drift from what it
   counts — the same shape as the four frozen chains', and asserted at the frontier's teardown. */
static long g_acct_live;

/* Drop a reference: refcount--, free at zero, continue into `up`. A LOOP, not recursion — the ancestry is as
   deep as the fork tree, which an unknown-length walk makes as deep as the walk is long, and the C stack cannot
   be parked. The twin of cow_seg_unref / dec_seg_unref / cons_seg_unref. */
static void acct_unref(FlowAcct *a) {
    while (a && --a->refcount <= 0) {
        FlowAcct *up = a->up;
        DCHECK(a->owner == NULL,
               "a fork-tree node was freed while the flow it accounts for is still in the frontier — that "
               "flow holds a reference of its own, so a zero here means the reference was dropped twice and "
               "that flow's next departure charges through freed memory");
        free(a);
        g_acct_live--;
        a = up;
    }
}

/* WHO A DEPARTING FLOW'S SERVICE BELONGS TO — the nearest LIVE ancestor of `a`, or NULL when every flow above it
   has gone too (then the chain is gone and there is no rank the charge could move). Compresses the dead run it
   walked onto `a->up`, so the traversal is amortised rather than repeated by every later arm of the same
   ancestry. Refs the new target BEFORE dropping the old link, so the unref cannot free the node it walked to. */
static Flow *acct_live_ancestor(FlowAcct *a) {
    FlowAcct *t = a->up;
    while (t && !t->owner && t->up) t = t->up;
    if (t && t != a->up) { t->refcount++; acct_unref(a->up); a->up = t; }
    return t ? t->owner : NULL;
}

/* THE SERVICE A DEPARTING FLOW BURNED, HANDED TO THE FLOW THAT FORKED IT — the mechanism the block above exists
   for. What is handed over is what this flow consumed BEYOND the prefix it inherited at its fork, so every
   microsecond of thread time is charged to the chain exactly once and no ancestor is billed twice for a prefix
   its own `cpu` already records. Called from flow_remove and nowhere else: leaving the frontier is the one event
   that makes a flow's own `cpu` unreadable, so it is the one moment the chain has to be told. */
static void acct_charge_departed(Flow *f) {
    int64_t residual = f->cpu - f->born;
    Flow *anc;

    DCHECK(f->acct != NULL && f->acct->owner == f,
           "a flow departed without owning its own fork-tree node — either it never got one, or another flow's "
           "node is standing in for it and the arms it shed were charged to a chain it was never part of");
    DCHECK(residual >= 0,
           "a flow departed owing NEGATIVE service to the flow that forked it — `born` is the service it "
           "inherited and `cpu` only ever grows above it (an emit clears both together), so this would REFUND "
           "the chain and promote a monopolizer once per arm that finishes");
    anc = acct_live_ancestor(f->acct);
    if (anc) anc->cpu += residual;
    f->acct->owner = NULL;   /* it has left the frontier: every later charge walks past it */
    acct_unref(f->acct);
    f->acct = NULL;
}

/* THE RANKING CHANGED, SO THE RUNNING FLOW'S CLAIM ON THE THREAD MAY HAVE. §scheduler's VALUE YIELD fires "the
   moment a parked flow outranks (or on an emit/fork/suspension that changes ranks)", and this is the moment: a
   sibling landed on the frontier, a flow left it, or the running flow emitted. Bumping the generation alone made
   the new answer AVAILABLE and left it unasked until the running flow happened to reach a suspend point — which,
   before the interpreter polled at every opcode, meant until the page happened to execute a loop back-edge. So
   the bump raises the engine's yield request: the running flow reaches its next opcode boundary, the poll asks
   preempt_hook, and the WFQ decides there. Raising is not deciding — a flow that still outranks everything keeps
   the thread and pays one declined hook call for the question. */
static void frontier_rank_changed(void) {
    g_gen++;
    JS_RequestFlowYield();
}

/* THE FRONTIER'S MEMBERS, in registry order. A walk, not a rank: the pending-fetch register lives on the flows,
   so whoever asks what the host owes has to visit all of them. flow_best answers a different question. */
Flow *flow_at(int i) { return (i >= 0 && i < g_flows_n) ? g_flows[i] : NULL; }

void flow_registry_init(const char *doc_name) {
    g_flows = NULL; g_flows_n = 0; g_flows_cap = 0; g_gen = 0; g_running = NULL;
    /* THE WORLD NAMESPACE COMES UP WITH THE FRONTIER, not beside it: every flow added below is minted a world
       named by this document, and a frontier whose worlds were unnamed could not be reached from another
       instance at all. */
    world_registry_init(doc_name);
}

unsigned flow_frontier_gen(void) { return g_gen; }

void  flow_set_running(Flow *f) { g_running = f; }
Flow *flow_running(void) { return g_running; }

/* Credit the running flow with newly EMITTED value (a new @H endpoint / @S PoC): its WFQ reward rises and its
   CPU-since-emit aging resets, so a productive flow outranks fresh + starved flows. Called by the detectors
   (endpoint.c / solve.c) when they record something NEW — this is what makes the WFQ value-of-information
   ordered rather than merely breadth-first by visit count. The frontier gen bumps so the value-yield re-ranks. */
void flow_credit_emit(double v) {
    /* ONE EMISSION IS ONE POINT, and the aging term's exchange rate is stated against exactly that. A credit of
       zero or less would leave a flow's rank unchanged while resetting its aging, which is the one way a
       monopolizer could refresh its lead without producing anything — so the reward's sign is asserted where it
       enters rather than assumed by the comparator that subtracts against it. */
    DCHECK(v > 0.0, "a flow was credited a non-positive emission — the WFQ's reward term counts NEW @H/@S "
                    "findings at one point each, and a zero credit resets the aging that outranks a monopolizer "
                    "while adding nothing to weigh it against");
    if (!g_running) return;
    /* THE INHERITED HALF MAY NEVER EXCEED THE WHOLE, asserted where the whole moves. `val_born` is what this
       flow was handed at its fork or its rebuild and `val` is what it holds now, so a flow standing above its
       own birth mark is one whose two writers ran in the wrong order — and the only symptom would be a census
       row reporting that nothing in the frontier has ever emitted anything. */
    DCHECK(g_running->val_born <= g_running->val,
           "a flow holds more inherited reward than reward — its birth mark was written after its account, so "
           "the census cannot tell what this flow emitted from what it was handed");
    g_running->val += v;
    g_running->cpu = 0;   /* emitted -> no longer a CPU-burning monopolizer */
    /* …AND THE BIRTH MARK WITH IT, because the mark is what this flow OWES the flow that forked it and the
       forgiveness has to be the same one. Left standing it would sit ABOVE `cpu`, and the residual charged at
       this flow's departure would be NEGATIVE — which does not merely lose the aging, it REFUNDS the chain and
       promotes it, the exact failure flow_age_running's own assertion is written against. */
    g_running->born = 0;
    frontier_rank_changed();   /* rank changed: re-rank at this flow's next opcode */
}

/* Age the running flow by the MICROSECONDS of thread time its step just burned. A monopolizer that runs without
   emitting sinks below productive + unrun flows — see FLOW_AGE_RATE for the exchange that makes that true.
   IT IS NOT THE ONLY CHARGE ON `cpu`: a DEPARTING flow hands what it burned to the flow that forked it (see
   FlowAcct), because the monopolizer this term is priced against is the fork CHAIN and not one of its N names.
   NO `if (g_running)` GUARD. There is exactly one caller and it charges between a switch-in and a finish, so a
   charge with nothing running would mean the scheduler lost track of which flow burned the time — and silently
   discarding it is how a monopolizer stops aging altogether. It crashes instead. */
void flow_age_running(int64_t us) {
    DCHECK(g_running != NULL,
           "thread time was charged with no flow running — the scheduler charges the flow it just stepped, so "
           "this is time nobody is billed for and a flow that never ages");
    /* THE CLOCK THE SCHEDULER AGES BY IS MONOTONIC, ASSERTED WHERE ITS READING ARRIVES. A negative charge does
       not merely lose the aging: it RAISES the flow's weight, so a flow that burns the thread would climb to the
       top of the frontier and stay there. That is the exact failure this term exists to prevent, and the only
       way to get one is a non-monotonic (or wrapping) clock at the call site. */
    DCHECK(us >= 0, "a NEGATIVE thread-time charge reached the WFQ — the aging clock ran backwards, which does "
                    "not slow a monopolizer down, it promotes it");
    g_running->cpu += us;
}

/* A FORKED SIBLING IS A CONTINUATION, NOT A NEWCOMER — so it inherits BOTH terms of its parent's account, and
 * this is the one place that says so.
 *
 * A fork is "append an arm to a decision vector" (flow.h): the sibling holds the parent's frame snapshot, the
 * parent's COW delta, the parent's DOM base, the parent's queued jobs and the parent's outstanding replies —
 * every field of the parent's HISTORY is copied at the fork except the two the WFQ ranks by, which were left at
 * the constructor's zeros. Both zeros are false statements about that flow, and each is false in the direction
 * that promotes it:
 *
 *   `val` — the sibling has, by construction, executed every emission the parent made before the branch; they
 *   are in its own delta and its own recipe replays them. Zeroing it says the flow that found six endpoints and
 *   then branched produced nothing, and ranks its two continuations below arms nobody has looked at. The cold
 *   tier already decided this question the other way at the OTHER place a flow is rebuilt from another's state:
 *   park_flow carries `val` across the park and states plainly that a replay re-emits and the resulting
 *   double-count is deliberate, because ORDER is the only thing a weight decides. A fork is the same rebuild
 *   and was answering it differently.
 *
 *   `cpu` — the sibling has burned the parent's CPU, through the parent, on the prefix it is resuming. Zeroing
 *   it hands a flow that has been running since boot a service notch of 0, which is the FULL optimism bonus
 *   (1.0) that flow_weight reserves for a flow the scheduler has never given the thread to. That is the hole
 *   the aging term cannot close from the other side: a flow that burns the thread without emitting sinks, but a
 *   flow that BRANCHES while burning it hands its debt to a child born debt-free, so a forking loop — the
 *   `Array.from(state.items)` walk FLOW_AGE_RATE was written against — ages forever and never sinks.
 *
 * WHAT THE TWO ZEROS DID TOGETHER, measured on the smoke fixture: every fork minted a flow at weight 1.0, the
 * maximum a val-0 flow can hold and strictly above every flow that had consumed a quantum (bonus <= 0.5, since
 * one notch of aging is 0.012 against half a point of optimism). So a newborn strictly outranked the whole
 * backlog, the value yield fired at the fork, and the thread went to the newest flow while every flow that had
 * already had one turn waited behind flows that did not exist yet. Forking replenishes the front of the queue
 * faster than a quantum drains it, so no flow ever got a SECOND turn: 9451 flows, 14126 switches (1.5 turns
 * each — one turn per flow plus one handoff per fork), `finished` 0, and a document whose deepest program index
 * was still 1. That is not breadth-first exploration; it is a queue that can only ever be entered.
 *
 * INHERITING PUTS THE FORK AT THE BACK RATHER THAN THE FRONT. The sibling ties with its parent, so it does not
 * dislodge it (flow_pick seeds the incumbent and takes a STRICT comparison), and it ranks BELOW every flow that
 * has consumed less service — which is exactly the backlog. This is start-time fair queueing's rule and not a
 * new policy: a continuation of an active flow enters at that flow's virtual time, never at the system's, or
 * any flow can reset its own virtual clock by splitting.
 *
 * WHAT IT DOES TO A FROM-BASELINE FLOW IS NOT WHAT THIS PARAGRAPH USED TO CLAIM, and the claim was checkable
 * against the formula three functions below it. It said "a genuinely from-baseline flow (the first flow, a
 * candidate session, a cold resume) still enters at zero and still outranks everything, which is what
 * §scheduler's 'a never-run flow is never starved' is about". Such a flow enters at reward 0 and service 0,
 * so its weight is EXACTLY 1.0 — the optimism term's whole range — and it outranks a flow of reward V only
 * once THAT flow's chain has burned (V − 1) / FLOW_AGE_RATE microseconds since its last emit: about a second
 * of unproductive thread time per point, for each such member, since aging is per flow. Inheritance makes
 * `val` monotone down every fork chain, so on a document whose exploration has emitted V findings the entire
 * tree below that prefix sits above 1.0 and a from-baseline flow is reached when that tree DRAINS — not when
 * the optimism term decides it is starved. That is a real ordering with a real justification (the tree is
 * where the emissions came from) and it is NOT the guarantee the deleted sentence made, so it is written down
 * as what it is. §scheduler's "never starved" is true among members of EQUAL reward and among no others.
 * WHICH OF THE TWO A RUN IS IN IS NOW A MEASUREMENT rather than either sentence: flow_wfq_census reports the
 * reward spread over the frontier and how many members sit at reward 0, and engine.c emits it as @WFQ beside
 * @PROGRESS and @COLD.
 *
 * AND INHERITANCE IS ONLY HALF OF IT — the other half is that the sibling stays ATTACHED. Copying the two terms
 * says where the arm enters; it says nothing about where the thread time the arm goes on to burn ends up, and
 * for an arm that runs the rest of the document and FINISHES the answer was nowhere. So this is also the line
 * that records the fork EDGE, and the two are one statement: a fork chain is one monopolizer wearing N names, so
 * it enters at one place and it is charged as one thing. See FlowAcct. */
void flow_fork_inherit(Flow *sib, const Flow *parent) {
    DCHECK(sib != NULL && parent != NULL && sib != parent,
           "a fork's accounting was inherited from nobody, or from itself — this is the one line that decides "
           "where a newborn arm enters the queue, so it cannot be asked about one flow");
    /* NOTHING MAY HAVE BEEN CREDITED OR CHARGED FIRST, because the inheritance ASSIGNS both terms. A caller
       that ran the sibling — or credited it — before handing it its account would have run it at a rank nobody
       chose, and the assignment would then silently erase whatever that run produced. */
    DCHECK(sib->val == 0.0 && sib->val_born == 0.0 && sib->cpu == 0 && sib->born == 0,
           "a forked sibling was credited or charged before it inherited its parent's account — it was ranked, "
           "and possibly run, at a weight that belongs to no flow, and this assignment throws that away");
    /* AND IT MAY NOT ALREADY STAND UNDER A FLOW IN THE FORK TREE. flow_new mints a root node, so a non-NULL
       `up` here is a second inheritance on one sibling: the first parent's node would be leaked and every arm
       this flow later sheds would be charged to the wrong chain. */
    DCHECK(sib->acct != NULL && sib->acct->up == NULL,
           "a forked sibling already stood under a flow in the fork tree — it is being given a second parent, "
           "so the service it sheds when it departs is charged to a chain it was never part of");
    sib->val = parent->val;
    /* …AND THE MARK THAT SAYS IT WAS INHERITED. `val` is the whole of the sibling's reward and none of it is
       its own, so this is the quantity the census subtracts to ask what a member has emitted since it began —
       the one question `val` cannot answer once it is copied down a fork chain. Written here, beside the copy
       it describes, so a term the fork carries cannot acquire one without acquiring the other. */
    sib->val_born = parent->val;
    sib->cpu = parent->cpu;
    /* THE BIRTH MARK: the service this sibling INHERITED, which the parent's own `cpu` above already records.
       What it hands back when it departs is what it burned BEYOND this, so the shared prefix is charged to the
       chain exactly once and no ancestor is billed twice for it. */
    sib->born = parent->cpu;
    /* AND THE EDGE ITSELF — the one line that makes a fork chain an accounting unit rather than N unrelated
       flows. It belongs here for the same reason the two terms above do: this is where a newborn arm's place in
       the ranking is decided, and its place is UNDER the flow it branched from. */
    sib->acct->up = parent->acct;
    parent->acct->refcount++;
    /* §scheduler'S ONE WFQ, ASSERTED AT THE FORK: A FORK IS RANK-NEUTRAL. The sibling is the same flow's path
       with one more arm on it, so at the instant it is born it is worth exactly what the parent is worth —
       branching is neither a promotion nor a demotion. It is not a tautology dressed as a check: it fires the
       moment anything else enters flow_weight that a fork does not carry, which is precisely the shape every
       banned fix for this defect takes (a depth bonus, a script-index term, a separate visit counter, a
       "prefer flows that have run" tiebreak). Any of those would make the two sides differ here, at the fork,
       instead of six minutes later in a progress line that says `finished 0`. */
    DCHECK(flow_weight(sib) == flow_weight(parent),
           "a fork changed the ranking — the sibling is the parent's path with one arm appended, so the two "
           "must be worth the same at the instant of the branch; a weight term the fork does not carry lets a "
           "flow change its own rank by branching, which is the one thing the WFQ may never let it do");
}

/* RELEASE ONE FLOW — see flow.h. This is where every field a Flow owns is given back, and the only place. */
void flow_release(JSContext *ctx, Flow *f) {
    /* THE SCHEDULER IS NOT SWITCHED INTO IT. Everything below is released as PARKED state — the heap delta's
       head is freed rather than unapplied, the DOM head's created nodes are destroyed on the assumption they
       are detached, and the decision/pin state is read out of blobs rather than out of decide.c's live globals.
       All three are false for the running flow, and each fails differently and late; asked once, here. */
    DCHECK(f != g_running,
           "the RUNNING flow was released — its heap delta, its document writes and its decision state are all "
           "live rather than parked, so this frees one copy of each while the scheduler still holds the other");
    /* A PAGED FLOW IS NOT A DROPPED ONE, which is the only thing this assert is about. A continuation left
       parked here is an async activation nobody will ever resume — UNLESS THIS FLOW was written to the cold
       tier, in which case its recipe replays the code that created that continuation and re-parks it in the
       next session. That is the whole claim the cold tier makes, so it is stated where the drop would
       otherwise be asserted rather than by teaching the assert to tolerate a NULL.
       IT ASKS THE FLOW, NOT THE ENGINE, and that is the correction a partial park forces. `engine_frontier_paged`
       answers for the whole session, so once ANY park had happened it excused the release of every flow —
       including one the park never wrote. A partial self-park releases a written TAIL while everything above it
       keeps running and stays unwritten, so the engine-wide answer is true and false at the same moment; the
       per-flow one is exact in both directions. */
    /* AND IT IS THE SAME QUESTION FOR ALL FIVE THINGS A RELEASED FLOW CAN STILL BE CARRYING, which is why they
       are ONE assert and not one assert plus four silent frees. `paged` was asked of the parked continuation
       alone, and the four things released below it — the suspended FRAME (this flow's whole heap-frame chain,
       every activation it is stopped across), the REPLIES the host still owed it, the JOBS on its queue, and
       its own loaded chunk BODIES — were given back with nothing saying whether they were work items being
       dropped. Every one of them is a work item on the ONE frontier, and §scheduler forbids dropping one; the
       justification written beside the frees is precisely the cold tier's ("the recipe re-enqueues the
       reactions and re-issues the requests as it replays"), which is the `paged` flag and nothing else. So a
       flow that was NOT written out may hold none of them, and the honest teardown of a session that ends over
       live flows either paged them or had nothing to lose.
       THIS IS THE ONE THAT FIRES ON THE SMOKE HOST'S GIVE-UP PATH — a provider that answers nothing ends
       run_scheduler, main.c tears the instance down, and flow_registry_free releases survivors that are
       suspended mid-frame with replies outstanding. That abort is the point: it names the flows a run threw
       away at the moment it threw them, which no counter, no census line and no result field has ever said. */
    DCHECK(f->paged || (f->park_fn == NULL && f->frame == NULL && f->njob == 0 &&
                        pending_count(f->pending) == 0),
           "a flow was released holding WORK — a parked continuation, a suspended frame, queued reactions or "
           "replies the host still owed it — and it was never written to the cold tier, so nothing replays it "
           "and every one of those is a work item the ONE frontier just dropped");
    /* AND THE PARK IS RELEASED HERE, BECAUSE THE SLOT OWNS IT. Three lines used to null these fields under
       the claim that "`opaque` is an activation reachable from the frame chain released just below, so what
       goes is the intention to resume it and not the memory." That was false at every one of the three sites
       that park — each takes a reference and only the resume gives it back — and false by construction for the
       flow this leak was found through, whose `frame` was NULL, so there was no chain below it to be reachable
       from. The disposer now rides the park (quickjs.h's JSFlowParkFreeFn) and each site states its own
       release next to where it takes it, so this line calls a function rather than knowing three shapes.
       `paged` DOES NOT EXCUSE IT, and conflating the two debts is how this survived. The assert above is about
       a dropped WORK ITEM, and the recipe genuinely does replay a paged flow's continuation — that argument is
       sound and unchanged. It says nothing about the MEMORY the old continuation still holds here, which no
       recipe frees and no gc_obj_list walk can see, because neither the activation nor its TrampFrames are GC
       objects. That was 4 surviving frames at flow_registry_free's census with nothing naming the owner. */
    DCHECK(!f->park_fn == !f->park_free,
           "a flow holds a parked continuation with no disposer, or a disposer with nothing parked — the pair "
           "is stored together at the switch out and required at the park itself, so one without the other is "
           "a park assembled outside JS_TakeParkedFlow");
    if (f->park_fn) {
        ((JSFlowParkFreeFn *)f->park_free)((JSContext *)f->park_ctx, f->park_opaque);
        f->park_ctx = NULL; f->park_fn = NULL; f->park_free = NULL; f->park_opaque = NULL;
    }
    DCHECK(f->park_ctx == NULL && f->park_opaque == NULL,
           "a flow holds half a park — the fields are written and taken together (JS_TakeParkedFlow), so one "
           "set without the others means a park was assembled outside that pair");
    /* `frame` is the JS_FlowNew handle holding this flow's whole heap-frame chain — every activation, closure
       and local it is suspended across — so one left behind retains the entire realm, and the runtime's leak
       walk reports it as a Window, a context and seventeen hundred anonymous Functions with nothing naming the
       owner. That is precisely the shape a leak takes when the ROOT is one dropped handle. */
    if (f->frame) { JS_FlowFree(ctx, (JSValue *)f->frame); f->frame = NULL; }
    /* the REPLIES it was still owed. Each entry is a request the host never answered; the whole register is one
       JS value, so its resolve capabilities, its reply values and every string in it go with one release. */
    pending_free(ctx, &f->pending);
    /* THE UNSTARTED OPERATIONS GO WITH IT — one release for the array and every [record, token] pair it names,
       exactly as the register above. That they are gone at all is asserted by the caller (flow_finish, the
       pager), because a dropped record is a peer suspended forever and freeing it quietly is how that stays
       invisible; this is the release, not the check. */
    JS_FreeValue(ctx, f->perform_q); f->perform_q = JS_UNDEFINED;
    cow_delta_release(ctx, (CowDelta *)f->delta); f->delta = NULL;
    /* THE HEAD BEFORE THE CHAIN BELOW IT: a head node inserted under a segment's node is that node's child, and
       a child must be freed before the parent it hangs under is freed deep. */
    if (f->dom) dom_buf_free(f->dom, f->dom_n);
    f->dom = NULL; f->dom_n = f->dom_cap = 0;
    dom_base_release(f->dom_base); f->dom_base = NULL;
    decide_blob_free(f->dec_blob); f->dec_blob = NULL;
    concolic_pins_blob_free(f->pin_blob); f->pin_blob = NULL;
    for (int k = 0; k < f->dyn_n; k++) { free(f->dyn[k]); free(f->dyn_token[k]); }
    free(f->dyn); f->dyn = NULL;
    free(f->dyn_cand); f->dyn_cand = NULL;
    free(f->dyn_doc); f->dyn_doc = NULL;
    free(f->dyn_token); f->dyn_token = NULL;
    free(f->dyn_pos); f->dyn_pos = NULL;
    f->dyn_n = f->dyn_cap = 0;
    for (int k = 0; k < f->njob; k++) {   /* any undrained microtask/task jobs */
        for (int a = 0; a < f->jobs[k].argc; a++) JS_FreeValue(ctx, f->jobs[k].argv[a]);
        free(f->jobs[k].argv);
    }
    free(f->jobs); f->jobs = NULL; f->njob = 0; f->jobcap = 0;
    /* AND ITS WORLD DIES WITH IT, WHICH IS THE ONE THING A RELEASE OWES SOMEBODY ELSE'S INSTANCE. Every other
       line above gives back memory this process owns; this one is the only state a released flow leaves in
       ANOTHER heap. A peer that has ever been posted to or read by this flow holds a COW segment keyed on its
       world, materialized on arrival and released only when told — so a flow that leaves the frontier with no
       announcement is a segment that peer carries for the rest of its process, and a peer holding one cannot
       park at all (cold.c refuses it, correctly: a foreign segment has no recipe of ours). The ancestors whose
       last live descendant this flow was die with it, which is why this is a list and not a name. */
    {
        const char *const *gone;
        int n = world_flow_gone(f->world, &gone);
        engine_notify_worlds_gone(ctx, gone, n);
    }
    flow_remove(ctx, f);
}

void flow_registry_free(JSContext *ctx) {
    /* A FLOW LEFT IN THE FRONTIER IS THE ORDINARY CASE, NOT AN ERROR: the session closes over its survivors by
       design (engine_sched_step's exhausted path leaves every host-owed flow alive, and in the product a parked
       flow OUTLIVES the session entirely). So this is the teardown for a flow that never finished, and it is
       the SAME operation as evicting one and as finishing one — which is why it is one call and not a fourteen-
       field list restated here. It used to be that list, it released four of the fields and left the rest, and
       the drift was invisible until a leak walk named a Window nobody owned. */
    /* WHO LEFT THEM, RECORDED AT THE RELEASE, because by the assert below every owner is gone. That census is
       a GATE and says only that frames survived — the reader stands at the wreckage with nothing naming the
       flow that made it, which is the same failure as an OOM that does not say what it was allocating. A flow's
       heap chain is its own (JS_FlowClone deep-copies rather than shares), so the count NOT dropping when a
       suspended flow is released is a fact about THAT flow, and this loop is the last moment it exists to be
       named.
       IT RECORDS RATHER THAN ASSERTS, and it calls nothing. "A released flow must drop a frame" is not
       something this file can claim today — flow_step's JS_FLOW_DETACHED arm hands a base to an awaited promise
       and leaves `frame` NULL on purpose — and a probe that reached for a serializer here could abort the
       teardown of every session over a question only this drive asks. So it reads four fields it already owns
       and carries them into the abort that already exists. */
#if APICLIENT_DEV
    int rel_i = 0, culprit = -1, culprit_frame = 0, culprit_park = 0, culprit_paged = 0, culprit_started = 0;
    JSRuntime *rrt = JS_GetRuntime(ctx);
#endif
    while (g_flows_n) {
#if APICLIENT_DEV
        Flow *rf = g_flows[0];
        int had_frame = rf->frame != NULL, had_park = rf->park_fn != NULL;
        int paged = rf->paged, started = rf->started;
        int before = JS_TrampFrameCount(rrt);

        flow_release(ctx, rf);
        /* THE LAST SUSPENDED FLOW WHOSE RELEASE FREED NO FRAME — the newest candidate wins, because a chain
           that outlives the frontier was left by the last release that failed to take one. */
        if ((had_frame || had_park) && JS_TrampFrameCount(rrt) == before) {
            culprit = rel_i; culprit_frame = had_frame; culprit_park = had_park;
            culprit_paged = paged; culprit_started = started;
        }
        rel_i++;
#else
        flow_release(ctx, g_flows[0]);
#endif
    }
    free(g_flows); g_flows = NULL; g_flows_cap = 0;
    /* AND THE WORLD NAMESPACE GOES DOWN AFTER THEM, WHICH IS THE ORDER RATHER THAN A DETAIL. It came up with
       the frontier and it outlives it by exactly one step, because releasing a flow ANNOUNCES the death of the
       world it held — the registry is what names that world, so freeing it first left every release walking a
       table that had already gone. Any segment this instance still holds for a PEER's world is then a foreign
       flow's state in this document: nothing else is going to free it, and it is malloc'd, so the runtime's GC
       walk would never name it. */
    world_registry_free(ctx);
    /* NOT ONE HEAP CALL FRAME AND NOT ONE STEP MACHINE MAY OUTLIVE THE FRONTIER, and this is the only point in
       the program where that is a decidable question. A TrampFrame and a suspended builtin's state are the two
       largest things in the @HEAP line's `unattributed` residual, they are invisible to the runtime's own
       gc_obj_list walk (neither is a GC object), and every one of them belongs to some flow's suspended chain —
       so with every flow released the census must be zero. It was NOT: JS_FlowFree could not tear down a
       DEEP-suspended flow at all, and each survivor took its whole activation chain with it into nothing with
       no counter anywhere naming the owner. The census exists precisely so that question has an answer; asking
       it here is what turns it into a gate rather than a number in a progress line. */
    {
        JSRuntime *rt = JS_GetRuntime(ctx);
#if APICLIENT_DEV
        char why[640];
        snprintf(why, sizeof why,
                 "the frontier is gone and %d heap CALL FRAME(S) are still live — a suspended chain that no "
                 "flow owns is unreachable memory, and every frame of it holds its locals, its closed cells "
                 "and its callee. %d flow(s) were released; the last SUSPENDED one whose release freed no "
                 "frame was #%d (frame=%d park=%d paged=%d started=%d). A -1 there means every suspended flow "
                 "did drop its chain, so these frames were never owned by one and the leak is upstream of the "
                 "frontier",
                 JS_TrampFrameCount(rt), rel_i, culprit, culprit_frame, culprit_park, culprit_paged,
                 culprit_started);
        DCHECK(JS_TrampFrameCount(rt) == 0, why);
#endif
        DCHECK(JS_StepMachineCount(rt) == 0,
               "the frontier is gone and STEP MACHINES are still live — a continuation-holding builtin's state "
               "outlived the flow that was suspended inside it, along with every argument it captured");
    }
    /* AND NOT ONE NODE OF THE FORK TREE, which is the same question the four frozen chains are asked below: every
       reference on a node is its own flow's or a live descendant's, and both are gone by here. It is also what
       makes acct_live_ancestor's compression checkable rather than argued — a node it re-pointed and failed to
       release holds the whole ancestry above it, and a node is not a GC object, so nothing else would ever
       name the owner. */
    DCHECK(g_acct_live == 0,
           "a FORK-TREE node outlived the frontier — its only references are a released flow's own and a live "
           "descendant's, so one still live is a reference nobody dropped and the whole ancestry it names is "
           "unreachable memory");
    /* AND NOT ONE FROZEN SEGMENT, which is the other side of the release above and the reason it can be trusted.
       A segment of the heap chain or the document chain is referenced ONLY by the deltas forked below it and by
       the segments above it — the world registry's foreign segments and the frontier's flows, both released by
       the two lines above — so with all of them gone the count must be zero, exactly as decide_free asserts for
       the third chain built on the same primitive. A non-zero one is a delta reference nobody dropped: pure
       garbage holding no live JSValue, invisible to the runtime's gc_obj_list walk, and previously visible only
       as a process that grew by a delta per flow. This is the assertion that makes cow_delta_release's and
       dom_base_release's refcount arithmetic checkable rather than argued. */
    {
        long segs = 0, entries = 0;
        cow_chain_stats(&segs, &entries);
        DCHECK(segs == 0,
               "a frozen HEAP segment outlived the frontier — every reference on the chain belongs to a delta "
               "the flows and the world registry have just released, so one still live is a delta nobody "
               "released and the whole history it names is unreachable memory");
        dom_cow_chain_stats(&segs, &entries);
        DCHECK(segs == 0,
               "a frozen DOCUMENT segment outlived the frontier — beyond the memory, it holds the nodes the "
               "flows below it created, so nothing will ever destroy them or take them out of the identity map "
               "that names them");
    }
    /* AND THE DECISION STATE THE FRONTIER STANDS ON, released HERE and not by each host. Every flow's parked
       vector is a reference on a shared frozen chain, and the running flow holds one more in decide.c's
       globals; the blobs went with the flows in the loop above, so this is the last of them. Putting it in the
       three hosts' teardowns instead would be the hand-copied list build.mjs warns about — a host that forgot
       the line would leak the whole chain with nothing to say so, which is exactly how the world registry's
       own release came to be missing from one of them. It belongs to the frontier, so it goes down with it. */
    decide_free();
    /* AND THE PATH CONSTRAINT THE SAME DECISIONS WERE MADE UNDER, which is the FOURTH chain built on the same
       refcounted immutable segment and the only one with no teardown at all. Its blobs went with the flows; the
       one reference left is the globals', held by whichever flow was switched out last — every session has
       leaked that flow's frozen constraint, its entries and every key and value string in them, with no counter
       naming an owner because a segment is not a GC object. Released here for the reason the decision chain is:
       it belongs to the frontier, and putting it in the three hosts' teardowns instead is the hand-copied list
       that has already drifted once. */
    concolic_clear_pins();
    {
        long segs = 0, entries = 0, bytes = 0;
        concolic_chain_stats(&segs, &entries, &bytes);
        DCHECK(segs == 0,
               "a frozen CONSTRAINT segment outlived the frontier — every reference on that chain is a parked "
               "flow's blob or the running flow's globals, and both are gone by here, so one still live is a "
               "blob nobody released and the whole narrowing it names is unreachable memory");
    }
    /* AND THE PARK DOCUMENT WRITTEN OUT OF THIS FRONTIER, released here for the same reason the decision chain
       is: it is the frontier's own residue, the host has already read it out of the result document by the
       time any teardown runs, and putting the free in each host's teardown instead would be the hand-copied
       list that has already drifted once. */
    cold_free();
    /* AND THE PENDING REGISTER'S INTERNED FIELD NAMES, for the same reason and in the same place: the corpus
       hosts take a runtime down and bring another up per file, so an atom left here is a handle into a freed
       table that the next session's first push would write through. */
    pending_free_ctx(ctx);
}

/* EVERY FLOW EVER CREATED. Reported beside the switch count for the same reason that one is: without it, a run
   that takes twenty minutes instead of three is a number with no decomposition — "the frontier grew" and "each
   flow got slower" look identical from outside, and they need opposite fixes. */
static long g_flows_created;
long flow_created_count(void) { return g_flows_created; }

/* THE ONE CONSTRUCTOR. Every flow is born here with a world already decided, so no flow can exist without one
   and no caller can hand one a second. */
static Flow *flow_new(JSContext *ctx, JSValueConst fn, WorldId w) {
    g_flows_created++;
    if (g_flows_n >= g_flows_cap) {
        int nc = g_flows_cap ? g_flows_cap * 2 : 32;
        /* THE REGISTRY IS WHAT THE SALE WALKS. This allocation can page a flow out (solver/reclaim.h), and the
           pager's own flow_remove swap-removes from THIS array — so the array, its count and its capacity must
           all still describe the buffer that exists for the whole of the ask. Publishing the doubled capacity
           first would advertise room the buffer does not have to the one caller guaranteed to be inside it. */
        Flow **nf = reclaim_realloc(g_flows, (size_t)nc * sizeof(Flow *));
        CHECK(nf, "flow_add: OOM growing the frontier — a dropped flow corrupts BFS exploration");
        g_flows = nf; g_flows_cap = nc;
    }
    Flow *f = reclaim_calloc(1, sizeof(Flow));
    CHECK(f, "flow_add: OOM allocating a flow — a dropped flow corrupts the frontier");
    f->fn = JS_DupValue(ctx, fn);
    /* THE PENDING REGISTER IS EMPTY, AND EMPTY IS NOT AN ARRAY. Most flows never park on anything, so
       allocating one per flow would put a JSObject on the heap for every member of a frontier that reached
       29550 on this fixture — and the blocked scan the preempt hook runs at every suspend point would then
       have to read a length instead of testing a tag. It is minted by the first push. */
    f->pending = JS_UNDEFINED;
    /* …AND THE OPERATION QUEUE, empty for the same reason and stated the same way: a peer asks this document
       nothing at all in most sessions, and JS_UNDEFINED is not what a calloc leaves behind. */
    f->perform_q = JS_UNDEFINED;
    /* …and the register's own context, named HERE because this is the one point every flow passes through.
       Putting it in each host's setup would be the hand-copied list build.mjs warns about. */
    pending_set_ctx(ctx);
    f->val = 0.0; f->cpu = 0; f->born = 0;
    /* …AND ITS PLACE IN THE FORK TREE, minted HERE because this is the one point every flow passes through, so
       no flow can exist without a node for its arms to be charged to. A from-baseline flow's node is a ROOT
       (`up` NULL); flow_fork_inherit is the only thing that ever attaches one under another. */
    f->acct = reclaim_calloc(1, sizeof *f->acct);
    CHECK(f->acct, "flow_add: OOM allocating a flow's fork-tree node — a flow whose arms cannot be charged back "
                   "lets a fork chain burn the thread forever without its rank ever moving");
    f->acct->owner = f; f->acct->refcount = 1;
    g_acct_live++;
    f->cand_src = NULL; f->cand_payload = NULL; f->cand_sink = NULL;
    f->last_compiled = -1;   /* nothing compiled yet; see the no-replay DCHECK at the compile site */
    f->world = w;
    g_flows[g_flows_n++] = f;
    frontier_rank_changed();   /* frontier changed: the newcomer may outrank the flow holding the thread */
    return f;
}

Flow *flow_add(JSContext *ctx, JSValueConst fn, WorldId parent) {
    /* THE WORLD IS MINTED HERE so no flow can exist without one. A fork passes the world it BRANCHED AT and the
       child records the edge, which is what lets another instance materialize this flow's segment by forking
       the nearest ancestor it already holds. A from-baseline flow passes WORLD_NONE and gets a root.
       `parent` IS THE FORK POINT AND NOT THE OTHER ARM'S WORLD: the arm that keeps running is re-minted as a
       child of the same name (engine_sibling_assemble), so the branch retires its own world and the two arms
       are siblings rather than ancestor-and-descendant — which is what makes "do these two senders contradict?"
       answerable at a peer. */
    return flow_new(ctx, fn, world_is_none(parent) ? world_mint() : world_mint_child(parent));
}

/* BLOCKED = holding an unanswered synchronous host request. Scanned rather than counted because a counter is
   a second representation of the same fact, and the two drift at exactly the sites (fork, drain, release) that
   are already the hardest to keep in step. The register is short — it is one flow's outstanding requests. */
int flow_blocked(const Flow *f) { return pending_blocked(f->pending); }

/* THE OPERATION QUEUE'S LENGTH — see flow.h. The context is the pending register's, for the reason that file
   gives for holding one at all: the callers that matter (the scheduler's pick, the teardown asserts) hold a
   Flow and nothing else. */
int flow_perform_pending(const Flow *f) {
    JSValue v;
    int n;

    if (!JS_IsObject(f->perform_q)) return 0;
    v = JS_GetPropertyStr(pending_ctx(), f->perform_q, "length");
    DCHECK(JS_VALUE_GET_TAG(v) == JS_TAG_INT,
           "a flow's operation queue has no small-integer length — it is the engine's own Array and nothing "
           "outside it appends");
    n = JS_VALUE_GET_INT(v);
    JS_FreeValue(pending_ctx(), v);
    return n;
}

/* …AND WHETHER IT OWES AN ANSWER AT ALL — see flow.h. Scanned rather than counted, exactly as flow_blocked is
   and for the same reason: a counter is a second representation that drifts at the fork, the start and the
   answer, which are the three sites hardest to keep in step. */
int flow_owes_answer(const Flow *f) {
    if (flow_perform_pending(f) > 0) return 1;
    for (int i = 0; i < f->dyn_n; i++)
        if (f->dyn_token[i]) return 1;
    return 0;
}

/* THE SERVICE QUANTUM — why the rank moves in steps rather than continuously.
 *
 * Aging was applied per SCHEDULER STEP, so the running flow's weight fell by a hair after every single step. Two
 * flows of equal value — the ordinary case, since most flows have emitted the same amount and very often none —
 * therefore traded places after ONE step each, forever. That is a fair-share scheduler with an infinitesimal
 * quantum, which thrashes by construction, and here every trade pays a full COW swap: the smoke fixture spent
 * 4.4 MILLION context switches on work that needed a few thousand, and the cost of each grew with the size of
 * the document, so a page with a few hundred DOM writes could not finish at all.
 *
 * The rate is UNCHANGED — a flow still ages by exactly cpu * FLOW_AGE_RATE in the long run, so which flow wins
 * over any real interval is the same decision it always was. What is gone is the sub-quantum RESOLUTION: rank
 * moves one notch per quantum of service, so the flow that holds the lead keeps it for a quantum and a tied
 * rival then takes its turn. Nothing is dropped, nothing is truncated and no flow is skipped — it is an ORDER
 * decision, which is the only thing the WFQ is allowed to be.
 *
 * QUANTISING IS NOT AN ARTEFACT OF THE OLD UNIT — it is MORE load-bearing now that the charge is a clock
 * reading. A step count moved the weight once per step; elapsed time moves it continuously, so without the
 * notch the running flow would fall below a tied rival between two consecutive consultations of the preempt
 * hook, which is the same thrash at a finer grain.
 *
 * AND THE NOTCH IS THE COOPERATIVE QUANTUM, not a number of its own. Rank moving faster than the thread can
 * actually be handed over IS the sub-quantum resolution described above, and ENGINE_QUANTUM_MS is the smallest
 * slice this scheduler ever hands anyone. A private constant here would be the hand-copied value build.mjs
 * warns about: the two would drift, and the drift would surface as thrash that neither file explained.
 *
 * AN EMIT STILL PREEMPTS IMMEDIATELY. flow_credit_emit zeroes cpu and bumps the generation, so a flow that
 * produces value jumps the queue within the quantum — the quantum bounds thrash between EQUALS, never the
 * response to something that actually changed the ranking. */
#define FLOW_SERVICE_US ((int64_t)ENGINE_QUANTUM_MS * 1000)

/* THE EXCHANGE RATE BETWEEN THE TWO TERMS, and it is the entire mechanism: 1e-6 POINTS PER MICROSECOND — ONE
 * EMITTED FINDING PER SECOND of thread time a flow holds without emitting one.
 *
 * THE NUMBER IS UNCHANGED AND ITS UNIT IS NOT, which is precisely the defect. It was points per SCHEDULER STEP,
 * and a step count cannot price a monopolizer at all: at 1e-6 per step, giving back ONE emission's worth of rank
 * took about 10^7 steps, so test_forced.c's unknown-length walk (`Array.from(state.items)`, one outcome fork per
 * position, emitting nothing further) held the thread for 227 SECONDS with `switches` still at 1 — every yield
 * offered, every yield declined, because its reward from earlier in the document outweighed an aging term
 * denominated in something the reward was not.
 *
 * WHY ONE SECOND PER FINDING. The rate has to price silence on the timescale at which silence is monopolising,
 * and it has to leave a flow that is still producing alone. It does both, exactly and checkably: a flow that has
 * emitted V findings and then stops falls below a never-run sibling — whose weight is its own reward plus the
 * full optimism bonus, hence at least 1.0 — after V + 1 seconds of unproductive thread time. Nothing holds the
 * thread for a silent second and keeps its lead; nothing productive is demoted for the step in which it emits.
 * Measured against the case above: 227 seconds of silence now costs 227 points, and no flow in any run of this
 * engine has emitted anything close to 227 findings, so the walk sinks in its first seconds instead of never.
 * That crossing is not left as prose — flow_best asserts it below, and it FIRES on the tree this replaced.
 *
 * AND THE UNPRODUCTIVE THREAD TIME IS THE CHAIN'S, which is the second half of the same defect and was measured
 * the same way. With the unit fixed, the walk above STILL never sank: it forks a "stop at n" arm per position and
 * each arm burns the rest of the document and DEPARTS, so the seconds this rate prices were billed to flows that
 * ceased to exist while the walker was charged only for the per-position ask. 22 of 26 probe rows on the minimal
 * fixture read 0 and were flat from sample 16 of 113. A departing flow now hands its service to the flow that
 * forked it (FlowAcct), so "V + 1 seconds of unproductive thread time" counts the seconds the CHAIN spent. */
#define FLOW_AGE_RATE 1e-6

/* HOW MANY WHOLE QUANTA OF THE THREAD THIS FLOW HAS ACTUALLY CONSUMED — the ONE quantised reading of `cpu`,
 * and the unit both terms of the weight are denominated in. It is public because it is what a rank CHANGE is
 * made of: between two notches a flow's weight cannot move except by an emission, which is exactly the
 * invariant engine.c's seam assertion checks. */
int64_t flow_service_notch(const Flow *f) { return f->cpu / FLOW_SERVICE_US; }

/* Anytime-bandit priority: reward + UCB optimism − CPU aging. Additive (never a value/cpu ratio — the aging
   term already yields value-per-CPU behaviour without the 0/0 degeneracy on an unrun flow). */
double flow_weight(const Flow *f) {
    double reward = f->val;
    /* OPTIMISM DECAYS WITH SERVICE, NOT WITH BEING PICKED — and SERVICE IS WHAT WAS ACTUALLY CONSUMED, which is
       a FLOOR of the cooperative quantum, the unit this scheduler hands the thread out in. Keyed on the
       DISPATCH COUNT it fell by 0.167 the first time the scheduler chose a flow, so the act of switching to a
       flow was enough to make it no longer the best one; keyed on service it moves because of WORK.
       IT WAS A CEILING, AND THE CEILING IS WHAT MADE THIS SCHEDULER STOP EXECUTING. `(cpu + Q - 1) / Q` puts
       ONE MICROSECOND of consumed CPU in the same notch as a whole quantum, so the first charge a flow ever
       takes — flow_age_running runs after its very first step — costs it 0.5, HALF the entire optimism range,
       while every flow that has not run yet and every flow that has just emitted (flow_credit_emit zeroes cpu)
       stands at 1.0. A flow that has run AT ALL is therefore STRICTLY outranked by every fresh fork and every
       recent emitter, so the value yield fires at its next back-edge, the pick hands the thread on, and that
       flow's own first charge does the same to it one step later. Measured on one document, two frozen
       worktrees, same box, same wall time: 1,286,199 context switches against 11,878, the frontier frozen at
       512 flows (the complete 2^9 fork tree of that document's decisions, nothing forked past it), NOT ONE flow
       ever finishing (live == flows at every sample), the heap unchanged across 1,288 progress samples and zero
       @S candidates found. A perfect round robin at one back-edge per flow: it satisfies every fairness rule in
       this file and executes nothing.
       THE GUARANTEE THE CEILING WAS ADDED FOR SURVIVES THE FLOOR, and the paragraph that argued otherwise had
       the mechanism right and the conclusion backwards. With a floor a never-run flow TIES with a flow that has
       burned a quantum-minus-one, and a tie does not dislodge the incumbent — the value yield asks whether a
       rival is STRICTLY greater, and flow_pick seeds its scan with the incumbent for the same reason. That is
       not starvation, it is the service quantum doing the one thing it exists to do: the incumbent holds the
       thread until it has consumed a whole quantum, at which point its notch advances, its bonus halves, and
       every never-run sibling outranks it STRICTLY and is picked. A never-run flow waits at most one quantum per
       flow ahead of it, which is what "never starved" can mean for a scheduler that hands the thread out in
       quanta at all — and it is the only reading under which "a top-ranked flow runs on at ~zero switch cost"
       (§scheduler) is true of anything. */
    int64_t served = flow_service_notch(f);
    double ucb     = 1.0 / (1.0 + (double)served);
    /* THE AGING, IN THE SAME CURRENCY AS THE REWARD ABOVE — see FLOW_AGE_RATE. It reads the SAME notch the
       optimism term does, which this line only now literally does: it computed its own floor beside a ceiling,
       so the two terms disagreed about what one unit of service was. A flow's weight is therefore constant
       between two service quanta, which is also what lets the preempt hook's cached rival stay exact at O(1)
       (engine.c) and what its seam assertion is written against. */
    double aging   = (double)served * ((double)FLOW_SERVICE_US * FLOW_AGE_RATE);
    return reward + ucb - aging;
}

/* THE CPU AT WHICH A FLOW IS GUARANTEED TO RANK BELOW ANY NEVER-RUN SIBLING — the crossing §scheduler's sentence
   claims exists, computed from the flow's OWN reward rather than a fixed threshold that could fire falsely on a
   very productive flow. A never-run flow carries the full optimism bonus and no aging, so its weight is its
   reward + 1.0 and therefore at least 1.0; a serviced flow's is at most val + 1.0 − aging, and aging is at least
   (cpu − one quantum) * RATE. Past this charge the second is at or below ZERO and the comparison cannot go the
   other way for any reward — the margin was half a point while the optimism term used a ceiling (a serviced
   flow's bonus could not exceed 0.5) and it is the never-run flow's own bonus now, which is what the assertion
   compares against and is why the crossing still closes strictly.
   AND THE `cpu` IT IS COMPARED AGAINST IS THE CHAIN'S, not one flow's — a walker whose arms burn the document and
   depart carries their service (FlowAcct), which is what makes this assertion able to catch the walk at all. Read
   against the walker's own sliver it was an identity that held for a family monopolising the thread for hours.
   `static inline` so release (where the DCHECK's condition is unevaluated) does not warn. */
static inline int64_t flow_cpu_to_sink(const Flow *f) {
    return (int64_t)((f->val + 1.0) / FLOW_AGE_RATE) + FLOW_SERVICE_US;
}

/* HOST-OWED MARKS — see flow.h for what a mark means and why the flow carries one at all.
 *
 * A GENERATION RATHER THAN A PER-FLOW FLAG, so clearing every mark is a single increment rather than a walk of
 * a frontier that has reached tens of thousands of members. A flow is marked by stamping the current
 * generation on it, and every stamp ages out the instant the generation moves. */
static unsigned g_owed_gen = 1;   /* NEVER 0: a fresh (calloc'd) flow must read as RUNNABLE, not as marked */

static int flow_host_owed(const Flow *f) { return f->owed_gen == g_owed_gen; }

int flow_host_owed_count(void) {
    int n = 0;
    for (int i = 0; i < g_flows_n; i++) if (flow_host_owed(g_flows[i])) n++;
    return n;
}

void flow_set_host_owed(Flow *f) {
    DCHECK(f != NULL, "a host-owed report arrived with no flow — the scheduler marks the flow it just stepped, "
                      "so this is a report about nobody, and the flow that made it is picked again immediately");
    /* A FLOW CANNOT ANSWER HOST-OWED TWICE WITH NO HOST EVENT IN BETWEEN, AND THAT IS THE WHOLE MECHANISM SAID
       AS A CHECK. A marked flow is out of the pick (flow_pick skips it as a candidate and refuses it as the
       seed), so the only way it can be stepped again — and therefore the only way it can report again — is if
       something CLEARED its mark. The clears are the host's own events, and nothing else may spell one.
       IT IS THE ASSERTION THIS SUBSYSTEM WAS MISSING WHEN IT WAS MEASURED. The clear used to run at the top of
       every slice, on the reasoning that "between two slices the host ran" — true of a slice that ended because
       the engine had nothing left to do, and FALSE of one that ended because its CPU quantum expired, which
       hands the thread to a host with nothing to answer. On a document whose whole frontier was blocked (512 of
       512), a slice marked the ~59 flows it had time for, the quantum cut it short, and the next slice
       re-admitted all of them: the sweep could never reach the end of the frontier, so the STALL was
       unreachable by construction and the engine swapped COW deltas 1.76 MILLION times without one flow
       finishing. Nothing said so — the frontier looked busy, and `blocked: 512` had to be read off a census
       and reasoned about. This line states it at the moment it happens, and names the laundering. */
    DCHECK(!flow_host_owed(f),
           "a flow reported host-owed AGAIN with no host event in between — a marked flow is out of the pick, "
           "so it can only have been stepped because its mark was cleared by something that cannot have "
           "answered it. The frontier can then never be fully marked, the STALL is unreachable, and the "
           "scheduler re-asks members it has already asked for as long as the run lasts");
    f->owed_gen = g_owed_gen;
}

/* THE HOST ANSWERED THIS FLOW — see flow.h for why the clear is per flow and per EVENT. */
void flow_clear_host_owed(Flow *f) {
    DCHECK(f != NULL, "a host event cleared the mark of no flow at all");
    f->owed_gen = 0;   /* never equal to g_owed_gen, which starts at 1 and only ever moves forward */
}

void flow_clear_host_owed_all(void) {
    /* THE WRAP IS HANDLED, NOT ARGUED AWAY. Four billion clears is a document-wide event each and it is not
       impossible, and a stamp that aliased the new generation would read as owed on a flow that is runnable —
       an exclusion lasting until the next clear, which is precisely the "skips ANY flow" §scheduler's razor
       forbids. Resetting the stamps costs one walk per wrap. */
    if (++g_owed_gen == 0) {
        for (int i = 0; i < g_flows_n; i++) g_flows[i]->owed_gen = 0;
        g_owed_gen = 1;
    }
}

/* THE ONE ORDER, ASKED THROUGH ONE SCAN. Four questions are put to the ranking — who should be running, who
   the running flow is defending against, what this document's best weight is for the host's Level-1 order, and
   which member the pager gives up first — and they are a SEED, FILTERS and a DIRECTION over ONE comparator,
   never separate rankings (§scheduler: ONE WFQ policy at both levels). Written out per question, the order
   would have a place to drift per copy and the monopolizer assertion below would guard whichever copy happened
   to carry it; written once, every pick is made under it.
   THE INCUMBENT IS THE SEED, NOT A CANDIDATE LIKE THE REST, and that is the difference between "which flow is
   best" and "which flow should be RUNNING". The scan takes a STRICT comparison, so seeding it with the flow
   that already holds the thread states the rule exactly: the thread moves only for a flow that is strictly
   better — the identical rule the preempt hook's value clause applies, now applied at the other end of the same
   decision. Without the seed the pick returned the first maximum in REGISTRY ORDER, so a field of flows on
   equal weight (the ordinary state of a frontier whose members have all emitted the same amount, which is very
   often none) handed the thread to g_flows[0] on every single iteration and paid a full COW delta swap for a
   ranking that had not changed at all. It is not a hysteresis margin, a minimum service or a switch budget:
   there is no number in it, and a strictly better flow takes the thread at the very next opcode. */
static Flow *flow_pick(const Flow *seed, const Flow *exclude, int runnable_only, int worst) {
    Flow *best = NULL; double bw = 0.0;
    /* A MEMBER WITH ZERO SERVICE: never run, or just emitted (flow_credit_emit zeroes cpu). Either way it
       carries the full optimism bonus and no aging, so its weight is its reward + 1.0 and hence at least 1.0 —
       which is the only property the assertion below needs, and it is the same property for both. */
    int unrun = 0;
    DCHECK(!(seed && worst), "the eviction tail was asked with an incumbent to defend — the seed states who "
                             "keeps the THREAD, and the flow the pager gives up is not a question about that");
    /* THE SEED IS ELIGIBLE ON THE SAME TERMS AS EVERY CANDIDATE — a member of the frontier, and not one that
       has told the scheduler it can make no progress. A flow that answered OWED is the one case where the
       incumbent must NOT keep the thread: keeping it is the spin the mark exists to end. */
    if (seed && flow_is_member(seed) && !(runnable_only && flow_host_owed(seed))) {
        best = (Flow *)seed; bw = flow_weight(seed);
        if (seed->cpu == 0) unrun = 1;
    }
    for (int i = 0; i < g_flows_n; i++) {
        double w;
        if (g_flows[i] == exclude || g_flows[i] == seed) continue;
        /* NOT A DROP AND NOT A DEPRIORITISATION: the flow keeps its weight, its place and every work item it
           holds, and it is picked again the moment anything could have answered it (flow_clear_host_owed). */
        if (runnable_only && flow_host_owed(g_flows[i])) continue;
        w = flow_weight(g_flows[i]);
        if (g_flows[i]->cpu == 0) unrun = 1;
        /* THE TAIL IS THIS SCAN READ THE OTHER WAY, which is the whole of why the pager has no ranking of its
           own. The flow the cold tier gives up first must be the flow the WFQ would have run last, or the
           engine would page out a member the scheduler still wanted and keep one it was starving; a size
           estimate, an age or a "cheapest to rebuild" score would be exactly that disagreement. One comparator,
           one scan, one direction bit. */
        if (!best || (worst ? w < bw : w > bw)) { best = g_flows[i]; bw = w; }
    }
    /* §scheduler'S SENTENCE, ASSERTED WHERE THE CHOICE IS MADE — "CPU-AGING so a monopolizer that burns CPU
       without emitting sinks below productive+unrun flows". This is the one line in the engine that decides
       which flow runs, so it is where the claim is either true or a comment. It is not a tautology dressed as a
       check: it FIRES on the tree this replaced, where the charge was one unit per scheduler step and the walk
       above was re-picked at every iteration for 227 seconds with unrun siblings waiting. It fires again on any
       future edit that clamps the aging term, caps it, or lets the reward grow with the CPU it is weighed
       against — which is exactly the shape a "fix" for the thrash this quantum handles would take. */
    DCHECK(worst || !best || !unrun || best->cpu < flow_cpu_to_sink(best),
           "the WFQ re-picked a flow whose fork chain has burned more thread time since its last emit than its "
           "entire accumulated reward is worth, while a never-run flow was waiting — the aging term is no longer "
           "commensurate with the reward it is subtracted from, so a monopolizer cannot sink");
    return best;
}

/* WHAT THE ORDERING IS MADE OF — see flow.h for what each row answers and why `val` alone cannot answer it.
 *
 * IT IS A SEPARATE SCAN AND NOT A FIFTH QUESTION FOR flow_pick, which is the opposite of the rule one function
 * up and is deliberate: flow_pick's four questions are all the ORDER, so they must share one comparator or
 * they can disagree about which flow runs. This one decides nothing and reads the order's INPUTS, so sharing
 * that scan would mean giving the pick accumulators it does not need and a shape a census could drift into
 * steering. It calls flow_weight for nothing at all — it reports the two terms, never their sum.
 *
 * `val_top` IS flow_best's OWN reward, so the top of the census and the top of the order are the same flow by
 * construction rather than by two scans agreeing. */
void flow_wfq_census(WfqCensus *out) {
    Flow *top;
    int i;

    DCHECK(out != NULL, "the WFQ was asked to report its ordering into nothing");
    out->members = g_flows_n;
    out->val_min = out->val_max = out->val_top = 0.0;
    out->val_zero = out->self_emit = out->unrun = 0;
    out->svc_max = 0;
    out->cand_members = out->cand_unrun = out->cand_dec_max = 0;
    out->cand_svc_max = 0;
    for (i = 0; i < g_flows_n; i++) {
        const Flow *f = g_flows[i];
        int64_t s = flow_service_notch(f);

        DCHECK(f->val_born <= f->val,
               "a member of the frontier holds more inherited reward than reward — its birth mark and its "
               "account disagree, so `self_emit` below counts something that was never an emission");
        if (i == 0 || f->val < out->val_min) out->val_min = f->val;
        if (i == 0 || f->val > out->val_max) out->val_max = f->val;
        if (f->val == 0.0) out->val_zero++;
        if (f->val > f->val_born) out->self_emit++;
        if (f->cpu == 0) out->unrun++;
        if (s > out->svc_max) out->svc_max = s;
        /* AND THE SAME TWO QUESTIONS ASKED OF THE CANDIDATES ALONE — see flow.h. `cand_src` is what a
           candidate session IS (the substitution it carries), so this is the population itself rather than a
           proxy for it. The decision-vector length comes from wherever this flow's decision state currently
           lives: a parked flow's blob, and decide.c's live globals for the one the scheduler is switched into
           — the same split cold.c's census makes, because there is only one place each can be. */
        if (f->cand_src) {
            long e = 0;

            out->cand_members++;
            if (f->cpu == 0) out->cand_unrun++;
            if (s > out->cand_svc_max) out->cand_svc_max = s;
            if (f->dec_blob) decide_blob_stats(f->dec_blob, &e, NULL);
            else if (f == g_running) decide_live_stats(&e, NULL);
            if (e > out->cand_dec_max) out->cand_dec_max = e;
        }
    }
    top = flow_best();
    if (top) out->val_top = top->val;
}

/* The four questions, each a seed, a filter or a direction over the one scan above. */
Flow *flow_best(void) { return flow_pick(NULL, NULL, 0, 0); }

/* WHICH FLOW SHOULD HOLD THE THREAD — the dispatch loop's pick, defending the incumbent on a tie. */
Flow *flow_next_to_run(const Flow *incumbent) { return flow_pick(incumbent, NULL, 1, 0); }

/* WHO THE INCUMBENT IS DEFENDING AGAINST — the same scan with the incumbent taken OUT rather than seeded in,
   because the hook applies the strict comparison itself. Asking it with the seed would answer `cur` and the
   value yield would compare a flow against itself. */
Flow *flow_rival_of(const Flow *cur) { return flow_pick(NULL, cur, 1, 0); }

/* IS `tail` THE LOWEST-WEIGHT CANDIDATE? — the assert's own scan below, deliberately NOT flow_pick's, and a
   FUNCTION rather than a loop at the call site for two reasons that are the same reason. A DCHECK condition
   must be side-effect-free, and this reads weights and returns a bool and does nothing else — no out-param, no
   accumulator the caller has to hold. And because it is evaluated only inside the condition, a release build
   never runs the scan at all: the cost is a dev-build scan on the reclaim path, which is reached only at the
   RAM floor. The ORDER function stays single (this calls flow_weight and nothing else); only the SCAN is
   independent, which is the whole point of a re-derivation. Ties pass — the question is whether anything is
   STRICTLY below the tail, since flow_pick is free to return either of two equal minima. */
static int flow_is_min_weight(const Flow *tail, const Flow *exclude) {
    double tw = flow_weight(tail);
    int i;
    for (i = 0; i < g_flows_n; i++) {
        if (g_flows[i] == exclude || g_flows[i] == tail) continue;
        if (flow_weight(g_flows[i]) < tw) return 0;
    }
    return 1;
}

Flow *flow_worst(const Flow *exclude) {
    Flow *tail = flow_pick(NULL, exclude, 0, 1);
    /* EVERY MEMBER, RUNNABLE OR NOT — and that is not an oversight in the filter, it is what eviction is about.
       A flow waiting on the host cannot use the thread, which is why the PICK skips it; it is still a snapshot
       occupying RAM, and it is the CHEAPEST thing in the frontier to page, because its recipe re-issues the
       request it is waiting on and gets today's answer instead. Filtering the tail by runnability would leave
       exactly the flows that cannot run holding the memory the flows that can need.
       THE ONE INVERSION THIS ALLOWS IS `exclude` ITSELF and it is unavoidable rather than tolerated: the flow
       the scheduler is switched into cannot be written out (its decision state is live in decide.c, its delta
       applied to the heap), so if IT is the lowest-weight member the tail taken is one that outranks it. It
       corrects itself at the next context switch, when that flow is parked like any other.

       ONE ORDERING, ASSERTED — AND THE FORM THAT STOOD HERE ASSERTED NOTHING. It read
       `flow_weight(tail) <= flow_weight(flow_best())`: the MINIMUM over this scan's candidates against the
       MAXIMUM over every member, and a minimum of a subset is never above a maximum of its superset. The
       condition therefore held for every frontier that can exist, in every build, and the sentence above it
       was checked by nothing at all — the stale-DFAIL shape exactly, authoritative to read and silent in
       fact. Comparing the tail against a head over the SAME set is no better: min <= max is the same
       arithmetic, so there is no version of "the tail may never outrank the head" that can fail.
       WHAT IS ACTUALLY AT RISK IS THE PAGER ACQUIRING A RANKING OF ITS OWN — a size estimate, an age, a
       cheapest-to-rebuild score — and that IS falsifiable: the tail must be the lowest-weight candidate, and
       the moment the eviction path stops answering with that minimum this re-derived scan disagrees with it.
       It is also what catches a flow_weight that is not a pure function of the flow, which the seeded scan
       above could never notice because it only ever asks each weight once. */
    DCHECK(!tail || flow_is_min_weight(tail, exclude),
           "the flow the pager chose to page out is not the lowest-weight member of the frontier — the tail and "
           "the head are two readings of ONE comparator, so a tail that is not the minimum means a second "
           "ranking has appeared and the engine is evicting work the WFQ had not finished with");
    return tail;
}

void flow_remove(JSContext *ctx, Flow *f) {
    /* WHAT THIS FUNCTION DOES NOT FREE, ASSERTED RATHER THAN ASSUMED. A Flow owns fourteen things and this
       releases five; the other nine belong to flow_release above, which is the ONE caller and the one place
       that knows a flow has to be switched OUT before its COW delta and DOM head can be given back. That split
       is correct and it is also invisible: nothing said so, so a second caller — an eviction, a cancelled
       candidate, a frontier trim — would compile, run, and leak the flow's entire delta, its DOM head, its
       queued jobs and its suspended frame with no counter naming the owner. That is precisely the shape the
       delta leak took (a per-flow allocation nobody freed, holding no live JSValue by then, so the runtime's own
       leak walk could not see it either).
       Asserted at the ROOT of the contract instead: a field added to Flow gets a line in flow_release and a line
       here, and a caller that skipped the release crashes at the removal instead of two hundred megabytes
       later. */
    DCHECK(f->delta == NULL && f->dom == NULL && f->dom_base == NULL,
           "a flow was removed with its COW state still attached — the heap delta and the DOM head must be "
           "UNAPPLIED from the live heap and document before they can be freed, which is why flow_finish owns "
           "that and this does not: removing it here drops both");
    DCHECK(f->frame == NULL && f->park_fn == NULL,
           "a flow was removed holding a live frame or a parked continuation — its whole activation chain, and "
           "everything those frames close over, would be retained by a handle nothing will ever free");
    DCHECK(f->njob == 0 && f->jobs == NULL && JS_IsUndefined(f->pending) && JS_IsUndefined(f->perform_q),
           "a flow was removed still holding queued jobs, pending host replies or unstarted cross-agent "
           "operations — each is a work item on the one frontier, and the WFQ may never drop one");
    DCHECK(f->dyn == NULL && f->dyn_cand == NULL && f->dyn_doc == NULL && f->dyn_token == NULL &&
           f->dyn_pos == NULL && f->dec_blob == NULL && f->pin_blob == NULL,
           "a flow was removed with its lazily-loaded chunk bodies or its suspended decision/pin blobs still "
           "attached — the flow's own allocations, freed by nothing else");
    for (int i = 0; i < g_flows_n; i++) {
        if (g_flows[i] == f) {
            JS_FreeValue(ctx, f->fn);
            free(f->deliver); free(f->deliver_origin);
            /* THE CANDIDATE IT WAS VERIFYING. Both strings are this flow's own copies, and the only other place
               that frees them is the frontier's teardown — which walks the flows that are STILL THERE, so a
               flow removed here took them with it into nothing. `cand_sink` is static text and is not one. */
            free(f->cand_src); free(f->cand_payload);
            /* AND THE THREAD TIME IT BURNED, WHICH IS NOT ITS OWN TO TAKE WITH IT. A flow that ran the rest of
               the document and then departed is one name of a fork chain that is still there, so its service
               goes to the flow that forked it before the node it hangs from is dropped. This is the ONE exit a
               Flow has, which is what makes it the one place the chain can be told. */
            acct_charge_departed(f);
            free(f);
            g_flows[i] = g_flows[--g_flows_n];   /* swap-remove; order is by weight, not position */
            frontier_rank_changed();   /* frontier changed */
            return;
        }
    }
    DFAIL("flow_remove: flow not in the registry — double remove or a dangling Flow*");
}

int flow_count(void) { return g_flows_n; }

int flow_is_member(const Flow *f) {
    for (int i = 0; i < g_flows_n; i++) if (g_flows[i] == f) return 1;
    return 0;
}
