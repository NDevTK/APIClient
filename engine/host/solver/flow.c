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
    /* THE FAMILY'S SERVICE SINCE ITS LAST EMISSION — the quantity flow_weight's aging term is now made of, and
       MEANINGFUL ONLY ON A ROOT (`up == NULL`). Every flow carries a direct `family` pointer at that root, so
       this is read and written in O(1) and never by walking.
       WHY IT REPLACED THE PER-ARM CHARGE. flow_fork_inherit COPIES the reward down every arm, so a family that
       emitted V and has N live arms presents V exactly N times; the aging that is supposed to cancel V was
       charged to whichever arm HELD THE THREAD, and the departed-arm hand-up below moved it only when an arm
       LEFT. The arms that stayed LIVE each carried only the prefix from their own fork point, so every one of
       them had to individually burn V / FLOW_AGE_RATE before it stopped outranking a reward-0 member: the
       thread time to reach one was N * V / RATE and not V / RATE, and §NO BOUNDS refuses to bound N.
       IT WAS INSTRUMENTED BEFORE IT WAS BUILT, and the number is why this exists rather than a paragraph.
       Smoke fixture, full document, 686 members: `svcMax` 1124 against `svcFamMax` 8910 — the family had burned
       7.93 times what its largest single arm showed, against an optimism term whose entire range is 1.0 and a
       `valMax` of 10.0. The eight members at reward 0 were the @S candidates and `candSvcMax` was 1: over a
       fifteen-minute run the whole security search got about one quantum. That is not scheduler polish, it is
       the reason the @S half reports nothing.
       SO THE TWO TERMS ARE NOW SYMMETRIC: the reward is the FAMILY's (copied at every fork) and the aging that
       cancels it is the FAMILY's (charged once however many names the family wears). §scheduler's sentence —
       "a monopolizer that burns CPU without emitting sinks below productive+unrun flows" — is about a
       monopolizer, and this file already said which one: "a fork chain is one monopolizer wearing N names, so
       it enters at one place and it is charged as one thing." It entered at one place and was charged as N. */
    int64_t fam_us;
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

/* SHORTEN THE RUN OF DEAD ANCESTORS ABOVE `a` — RETENTION, and no longer an answer to anything. It used to be
   `acct_live_ancestor`, which found the nearest live ancestor because a departing flow's residual service had
   to be charged somewhere; there is no such charge any more (see acct_depart), and what is left is the reason
   it compressed: without this a family whose arms depart one at a time retains one node per departed arm for as
   long as any descendant lives, which for the unknown-length walk this file is written against is one node per
   iteration.
   IT STOPS AT THE ROOT — `t->up` non-NULL is the loop condition — and the root is the node every member of the
   family points its `family` tag at, so no compression can ever move a tag or orphan one. Refs the new target
   BEFORE dropping the old link, so the unref cannot free the node it walked to. */
static void acct_compress_dead(FlowAcct *a) {
    FlowAcct *t = a->up;
    while (t && !t->owner && t->up) t = t->up;
    if (t && t != a->up) { t->refcount++; acct_unref(a->up); a->up = t; }
}

/* A FLOW LEAVES THE FRONTIER — the node's LIFETIME and nothing else. What used to stand here also handed this
   flow's residual service (`cpu - born`) to its nearest live ancestor, and that is DELETED rather than kept
   beside the family charge: with `fam_us` counting every microsecond the family burns AS IT BURNS IT, a hand-up
   at departure would bill the family a second time for the same thread time and REFUND nothing — it would age
   a productive family for the work of every arm that has already finished, twice. `born` existed only to
   compute that residual and is gone with it, and so is `acct_live_ancestor`'s answer to "who to charge": there
   is no per-arm charge left to route. What the departure still owes the tree is the compression above, which
   is retention and not accounting — see acct_compress_dead. */
static void acct_depart(Flow *f) {
    DCHECK(f->acct != NULL && f->acct->owner == f,
           "a flow departed without owning its own fork-tree node — either it never got one, or another flow's "
           "node is standing in for it, and the family this flow's arms are charged to is not its own");
    f->acct->owner = NULL;   /* it has left the frontier: a later compression walks past it */
    acct_compress_dead(f->acct);
    acct_unref(f->acct);
    f->acct = NULL;
    /* THE TAG GOES WITH IT. The root node stays alive while any descendant's `up` chain refs it, so a live
       flow's `family` is never dangling; a departed flow's would be, the moment the last of its family goes. */
    f->family = NULL;
}

/* THE THREAD TIME THIS FLOW'S WHOLE FORK FAMILY HAS BURNED SINCE ITS LAST EMISSION — one indirection, because
   every flow points STRAIGHT at its family's root (`family`, joined at flow_fork_inherit) rather than walking
   an ancestry whose length is the fork depth. flow_weight calls this once per member per pick and flow_weight
   is itself evaluated inside DCHECK conditions, so this is READ-ONLY: a compressing find here would make every
   assertion in this file mutate the structure it is asking about. */
static int64_t acct_family_us(const Flow *f) {
    return f->family ? f->family->fam_us : 0;   /* departed: its node is gone and so is its rank */
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
    g_running->cpu = 0;   /* emitted -> this flow's own half of the aging is forgiven with the family's below */
    /* …AND THE OPTIMISM TERM'S OWN QUANTITY, which is what the line above used to be doing under a comment that
       said so ("a fresh visit count") while `cpu` was a clock. A flow that has just produced something is not
       one the frontier needs protecting from, so it is restored to the full bonus exactly as a never-run flow
       carries it — and it then leads by its REWARD, which is the term that is supposed to decide between two
       flows the optimism term has nothing left to say about. Without this reset a productive flow's bonus
       decays monotonically for the whole session and the only thing keeping it in front is `val`, so the moment
       it stops emitting it falls behind every arm it forked while it was working. */
    g_running->visits = 0;
    /* …AND THE FAMILY'S AGING WITH IT, which is the forgiveness that actually moves a rank now. The reward this
       emission raises is the FAMILY's — flow_fork_inherit copies `val` to every arm — so the aging it is
       weighed against has to be forgiven at the same granularity, or an arm that emits carries the silence of
       every sibling that did not and the credit buys it nothing. §scheduler prices this term against "a
       monopolizer that burns CPU WITHOUT EMITTING", and a family one of whose arms just emitted is not one.
       IT RAISES EVERY MEMBER OF THE FAMILY AT ONCE, and that is the same statement as the reward's, read the
       other way: the family is one accounting unit or it is not one at all. The gen bump below is what makes
       the frontier re-rank on it, and it is why the value yield's own assertion snapshots the family notch
       (engine.c) rather than this flow's alone. */
    g_running->family->fam_us = 0;
    frontier_rank_changed();   /* rank changed: re-rank at this flow's next opcode */
}

/* THE OTHER KIND OF QUANTITY IN THE WEIGHT, and the whole of why it is a separate function from the one above.
 * flow_credit_emit is a LEDGER entry: it records that something was LEARNED, so it is paid once and adds. This
 * is a COMPARATOR reading: it records where a candidate STANDS, so it is written, not added, and it says the
 * same thing however many times it is taken. Conflating them is not a style question — a distance paid into a
 * ledger has to be paid at most once per search to keep the ledger honest, and that is exactly the rule that
 * makes every candidate after the first indistinguishable from one that never started.
 * MONOTONE, AND THAT IS THE INVARIANT AND NOT A CLAMP. A flow's payload is fixed for its life, so the longest
 * run of it any re-execution has delivered can only be discovered, never undone: a later observation on a
 * different string is another sample of the same fixed question, and taking the smaller one would let a page
 * that writes the payload twice demote the flow for its second write. So a lower reading is DISCARDED as the
 * non-observation it is, and the early return is what keeps the generation from moving for a rank that did not.
 * NOT KEYED ON THE RUNNING FLOW, unlike the credit above: the observation is made about a specific flow's own
 * bytes and the caller already holds that flow, so passing it is what stops this becoming a second way to ask
 * which flow is running. */
void flow_set_distance(Flow *f, double d) {
    DCHECK(f != NULL,
           "an @S candidate's travelled distance was recorded against no flow — the distance is a fact about "
           "ONE flow's own payload, so a caller with no flow measured somebody else's bytes");
    DCHECK(d >= 0.0 && d <= 1.0,
           "an @S candidate's travelled distance is not a fraction — it is the surviving run over the payload's "
           "own length, so a value outside [0,1] is a run measured against a denominator that is not this "
           "candidate's, and it enters the weight beside an optimism term whose entire range is 1.0");
    DCHECK(f->cand_payload != NULL,
           "a travelled distance was recorded for a flow carrying no payload — the fraction's denominator IS "
           "the payload, so a flow with none has nothing this number could be a fraction of and would rank "
           "above every exploration flow on a measurement of nothing");
    if (d <= f->cand_dist) return;   /* not an improvement: the rank did not move, so nothing may re-rank on it */
    f->cand_dist = d;
    frontier_rank_changed();
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
    /* …AND THE SAME MICROSECONDS ON THE FAMILY. Both are the AGING term's — it is their SUM — and they are not
       two copies of one number, because they have two reset points: `cpu` is what THIS FLOW has burned since
       its OWN last emission, while `fam_us` is what the whole fork family has burned since ANY of its arms last
       emitted. The first is what orders a family's members against each other and the second is what orders one
       family against another, and a term with only the second has nothing to say on a frontier that is a single
       family — which a real page's is. There is deliberately no ordering between the resets: a sibling's
       emission forgives the family while this flow's own `cpu` stands, and that is the policy rather than a
       drift. Neither of them is the OPTIMISM term's quantity; that one is a count of completed units of work
       and is charged by flow_credit_visit, which is why it is a separate call from this one. */
    DCHECK(g_running->family != NULL && g_running->family->up == NULL,
           "the running flow's family tag is not the ROOT of a fork tree — an arm whose tag points at its own "
           "node has FOUNDED a family instead of joining its parent's, which is precisely the reset by "
           "splitting the whole of this accounting exists to forbid: it would carry its parent's reward with "
           "none of its parent's aging and outrank the entire backlog for free");
    g_running->family->fam_us += us;
    /* THE RESOLUTION THIS CHARGE MUST HAVE — that a slice of the thread MOVES the rank it is charged to — is
       asserted at the seam where the charge meets the pick, in engine.c's scheduler loop, because that is the
       one place both are visible and the claim is about their ORDER. See §scheduler's monopolizer sentence
       there, and FLOW_SILENCE_US here for what a charge the pick cannot see cost, measured. */
}

/* A FLOW COMPLETED A UNIT OF WORK — the OPTIMISM term's quantity, and the one thing in this file that is a count
   rather than a clock. See flow.h's `visits` for what a unit is and why the term may not be thread time.
   IT IS A SEPARATE CALL FROM flow_age_running AND THAT IS THE POINT. The two are charged at the same moment by
   the same caller and it would be one function if they measured one thing; they measure the two quantities
   §scheduler names separately, and collapsing them is exactly the substitution that made the aging term
   unreachable. A step that burned a quantum inside a program charges the first and NOT the second.
   THE FLOW IS NOT ASSERTED TO BE THE RUNNING ONE, unlike the charge above: the scheduler credits the flow it
   just stepped, and the finish path can have cleared `g_running` by then. What IS asserted is the property that
   makes the count a trial count. */
void flow_credit_visit(Flow *f) {
    DCHECK(f != NULL, "a completed unit of work was credited to no flow — the optimism term's whole quantity is "
                      "per flow, so a charge with no owner is a turn nobody is recorded as having had");
    DCHECK(f->frame == NULL,
           "a flow was credited a completed unit of work while it is still INSIDE a program — a preempt is the "
           "middle of a trial, not the end of one, and counting it makes this term a second clock beside the "
           "aging: the flow is then outranked by its own arms after one quantum and no program ever finishes");
    /* …AND A MICROTASK CHECKPOINT IT STILL OWES IS THE SAME SENTENCE, WHICH IS WHERE THE UNIT ACTUALLY ENDS.
       HTML §8.1.4.4 "Calling scripts", clean up after running script step 3: "If the JavaScript execution
       context stack is now empty, perform a microtask checkpoint" — the checkpoint is a step OF cleaning up
       after the script, run in the same turn, and §8.1.7.3 "Processing model" places the identical step at the
       end of a task. The checkpoint itself is "While the event loop's microtask queue is not empty: … Run
       oldestMicrotask", so the turn is over when the queue is EMPTY and not when the program returned. An
       empty execution context stack is therefore the checkpoint's TRIGGER, never the unit's end, and this
       counted it as the end.
       WHAT THAT COST IS THE WHOLE PRODUCT. The credit is a DEMOTION — the optimism term is 1/(1+visits) — so a
       flow was demoted at the exact instant its queued reactions first became eligible to run (every job arm of
       flow_step is under `frame == NULL`, engine.c). It then had to win the thread a SECOND time against the
       entire frontier before its own checkpoint could run, and on a forking page the frontier is refilled by
       branching faster than it drains. Measured on the artifact this fixes, three runs each: a six-line fixture
       whose only forking construct is a 40-iteration loop over `location.hash` reported 19678 flows, 1320 jobs
       QUEUED, 1316 units credited and ZERO jobs run — the two counts within four of each other because each
       flow that finished its program queued exactly one reaction, was credited exactly one unit, and never held
       the thread again. The same page without the loop (4 flows) ran 28 of 28 jobs. excalidraw.com: 15993-17556
       flows, 2657-2923 jobs queued, 3500-4067 units, jobsRun 0, endpoints 0 on all three runs.
       IT IS NOT A DRAIN LOOP AND MAY NOT BECOME ONE. Each microtask is still its own flow_step return, still
       preemptible, still parkable, still re-ranked; what this removes is only the flow's own demotion ACROSS
       the checkpoint, so flow_pick's strict comparison leaves the thread with it on a tie and any strictly
       better flow still takes it. A checkpoint that never ends is governed by the aging term, which is the term
       §scheduler assigns that job — not by a cap here. */
    DCHECK(!flow_job_microtask(f),
           "a flow was credited a completed unit of work while it still owes a MICROTASK CHECKPOINT — HTML "
           "§8.1.4.4 \"Calling scripts\" step 3 of clean up after running script performs the checkpoint as "
           "part of the turn that just ended, so the unit is not over and this credit DEMOTES the flow at the "
           "one instant its queued reactions became eligible to run: it must then out-rank the whole frontier "
           "a second time to run its own checkpoint, which on a forking page never happens");
    f->visits++;
}

/* A FORKED SIBLING IS A CONTINUATION, NOT A NEWCOMER — so it inherits EVERY term of its parent's account, and
 * this is the one place that says so.
 *
 * A fork is "append an arm to a decision vector" (flow.h): the sibling holds the parent's frame snapshot, the
 * parent's COW delta, the parent's DOM base, the parent's queued jobs and the parent's outstanding replies —
 * every field of the parent's HISTORY is copied at the fork except the ones the WFQ ranks by, which were left at
 * the constructor's zeros. Each zero is a false statement about that flow, and each is false in the direction
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
 *   it hands a flow that has been running since boot a silence of ZERO, which is the aging the term reserves
 *   for a flow that has just emitted. That is the hole the aging term cannot close from the other side: a flow
 *   that burns the thread without emitting sinks, but a flow that BRANCHES while burning it hands its debt to a
 *   child born debt-free, so a forking loop — the `Array.from(state.items)` walk FLOW_AGE_RATE was written
 *   against — ages forever and never sinks.
 *
 *   `visits` — the sibling has, by construction, completed every unit of work its parent completed before the
 *   branch, so zeroing it hands a flow that has finished programs the FULL optimism bonus (1.0) that
 *   flow_weight reserves for a flow the scheduler has never picked. It is the same reset-by-splitting as the
 *   `cpu` zero, arriving through the term that replaced the clock, and it is the one the aging cannot reach at
 *   all: a flow inside a program completes no unit, so a bonus it can refresh by branching would never decay.
 *
 * WHAT THE ZEROS DID TOGETHER, measured on the smoke fixture: every fork minted a flow at weight 1.0, the
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
    DCHECK(sib->val == 0.0 && sib->val_born == 0.0 && sib->cpu == 0 && sib->visits == 0 &&
           sib->cand_dist == 0.0 &&
           sib->family == sib->acct && sib->family->fam_us == 0,
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
    /* …AND THE COMPLETED UNITS, which is the optimism term's coordinate and the same sentence a third time: an
       arm has, by construction, finished every program, job, delivery and lifecycle stage its parent finished
       before the branch, because it IS that execution with one more arm on it. Zeroing it would hand a flow
       that has been working since boot the bonus reserved for one the scheduler has never picked, and — unlike
       the `cpu` zero this file already describes — the aging term could not close it from the other side,
       because a flow that never finishes a unit never decays at all. Branch-to-reset-your-own-optimism is the
       same reset-by-splitting the family charge forbids, arriving through the term that replaced the clock. */
    sib->visits = parent->visits;
    /* …AND THE DISTANCE, which is the same sentence about the term that is not a ledger. An arm of a candidate
       carries the SAME payload to the SAME sink (engine.c copies the substitution to the sibling), so it stands
       exactly where its parent stood: the bytes that survived to a sink survived on the prefix both arms share.
       Zeroing it would let a candidate refresh its own rank by branching — the reset-by-splitting this
       accounting forbids everywhere else, arriving through the one term that is read rather than accumulated —
       and the rank-neutrality assertion below is what catches it. For a NON-candidate parent this copies zero
       to zero, which is the truth about an arm of a flow that has no payload. */
    sib->cand_dist = parent->cand_dist;
    /* AND THE FAMILY IT JOINS — the term the AGING reads, and the one line that makes "a fork chain is one
       monopolizer wearing N names" true of the arithmetic rather than only of this comment. The arm does not
       FOUND a family: flow_new minted it a root of its own (that is what the precondition above checks), and
       this hands that root back and points the arm at its parent's. Every microsecond any arm of the family
       burns from here on is charged once, to this node, and read by every arm.
       ITS OWN NODE IS STILL THE FORK EDGE and is still refcounted — the two are different structures answering
       different questions: `acct`/`up` is the ANCESTRY (who forked whom, and what keeps a root addressable),
       `family` is the ACCOUNT. Collapsing them would mean walking the ancestry on every flow_weight, and
       flow_weight is evaluated inside DCHECK conditions where a walk that compresses is forbidden. */
    sib->family = parent->family;
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
       instead of six minutes later in a progress line that says `finished 0`.
       RE-DERIVED FOR EVERY TERM, AND IT IS NOT AN ASSUMPTION CARRIED OVER. flow_weight is now
       `val + cand_dist + 1/(1+visits) − (cpu + fam_us)*RATE`, so a fork must carry FIVE things for this to hold
       and it carries exactly five: `val` (copied above), `cand_dist` (copied above — the fitness comparator's),
       `visits` (copied above — the optimism term's), `cpu` (copied above — the aging's own half), and the
       family tag (joined above — the aging's other half). Each addition
       to the formula has arrived through this line firing, which is the whole reason it is written as an
       equality over flow_weight rather than as a list of fields. This assertion is what FORCES the join: had the arm kept
       the fresh root flow_new minted it, its `fam_us` would be 0 against a parent family that has burned, so
       the sibling would be born STRICTLY better than its parent and this line would fire at the very first
       fork. That is the reset-by-splitting the whole of this accounting forbids, and it is caught here rather
       than in a census that says the frontier never sinks. */
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
    /* A ROUTED DELIVERY IS ON THAT LIST NOW, and it was the one work item this walk never mentioned: flow_remove
       silently `free`d the record and the sender origin, so a peer's message died here with nothing to say so —
       the precise shape §Offensive-programming calls the concealment rather than the symptom. It belongs behind
       `paged` like the rest: the cold tier writes the queue as 'm' records, so a WRITTEN flow's deliveries are
       in its recipe and an unwritten one's are being dropped. */
    DCHECK(f->paged || (f->parked == NULL && f->frame == NULL && flow_job_pending(f) == 0 &&
                        pending_count(f->pending) == 0 && flow_deliver_pending(f) == 0),
           "a flow was released holding WORK — a parked continuation, a suspended frame, queued reactions, "
           "replies the host still owed it, or a peer's routed message — and it was never written to the cold "
           "tier, so nothing replays it and every one of those is a work item the ONE frontier just dropped");
    /* AND THE PARKED CONTINUATIONS ARE RELEASED HERE, BECAUSE THE FLOW OWNS THEM. Three lines used to null a
       set of raw fields under the claim that "`opaque` is an activation reachable from the frame chain
       released just below, so what goes is the intention to resume it and not the memory." That was false at
       every one of the sites that park — each takes a reference and only the resume gives it back — and false
       by construction for the flow this leak was found through, whose `frame` was NULL, so there was no chain
       below it to be reachable from. Each park's disposer rides the park (quickjs.h's JSFlowParkFreeFn) and
       each site states its own release next to where it takes it, so this line calls one function over the
       whole set rather than knowing three shapes and one park.
       `paged` DOES NOT EXCUSE IT, and conflating the two debts is how this survived. The assert above is about
       a dropped WORK ITEM, and the recipe genuinely does replay a paged flow's continuation — that argument is
       sound and unchanged. It says nothing about the MEMORY the old continuations still hold here, which no
       recipe frees and no gc_obj_list walk can see, because neither the activations nor their TrampFrames are
       GC objects. That was 4 surviving frames at flow_registry_free's census with nothing naming the owner.
       The half-a-park assert that stood here is GONE with the fields it read: a set is one pointer, so there
       are no halves to disagree, and what it was really guarding — that every member carries a resume, a
       disposer and a realm — is asserted over the whole ring, at the two lines that can walk it
       (JS_PutParkedFlows and JS_FreeParkedFlows). */
    JS_FreeParkedFlows(f->parked);
    f->parked = NULL;
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
    /* AND THE ROUTED DELIVERIES, one release for the array and every [record, senderOrigin] pair it names. That
       they are gone at all is asserted above and by the caller, never cleaned up quietly here. */
    JS_FreeValue(ctx, f->deliver_q); f->deliver_q = JS_UNDEFINED;
    /* AND WHICH SENDING TIMELINES IT WAS IN — one release for the array and every [vector, taken] pair it
       names. It is NOT on the work-item list the assert above refuses to drop, and that is the distinction
       rather than an omission: a commitment is not work owed to anybody, it is what this timeline WAS. A flow
       that has ended has no timeline left to be wrong about. */
    JS_FreeValue(ctx, f->deliver_world_q); f->deliver_world_q = JS_UNDEFINED;
    cow_delta_release(ctx, (CowDelta *)f->delta); f->delta = NULL;
    /* THE HEAD BEFORE THE CHAIN BELOW IT: a head node inserted under a segment's node is that node's child, and
       a child must be freed before the parent it hangs under is freed deep. */
    if (f->dom) dom_buf_free(f->dom, f->dom_n);
    f->dom = NULL; f->dom_n = f->dom_cap = 0;
    dom_base_release(f->dom_base); f->dom_base = NULL;
    decide_blob_free(f->dec_blob); f->dec_blob = NULL;
    concolic_pins_blob_free(f->pin_blob); f->pin_blob = NULL;
    /* THE BODY COLUMN GIVES A REFERENCE BACK, IT DOES NOT FREE. A program's text is shared by every timeline
       holding that program (solver/dyn_body.h), so what a flow owns is one reference and not the buffer; the
       last flow to leave is the one that frees it, and which flow that is, is not knowable here. The other two
       columns are this flow's own strings and are freed. */
    for (int k = 0; k < f->dyn_n; k++) { dyn_body_unref(f->dyn[k]); free(f->dyn_token[k]); free(f->dyn_url[k]); }
    free(f->dyn); f->dyn = NULL;
    free(f->dyn_cand); f->dyn_cand = NULL;
    free(f->dyn_type); f->dyn_type = NULL;
    free(f->dyn_url); f->dyn_url = NULL;
    /* THE ELEMENT COLUMN IS THE ONE THAT FREES NOTHING BUT ITSELF — a row's `script` element is a BORROWED node
       of the document's tree, owned by that tree (or, for a node a flow created, by that flow's DOM delta, which
       dom_buf_free above has already discarded). Freeing an entry here would destroy a node the document still
       holds; leaking one is impossible because there is nothing to leak. */
    free(f->dyn_el); f->dyn_el = NULL;
    free(f->dyn_doc); f->dyn_doc = NULL;
    free(f->dyn_token); f->dyn_token = NULL;
    free(f->dyn_pos); f->dyn_pos = NULL;
    f->dyn_n = f->dyn_cap = 0;
    /* AND THE UNDRAINED MICROTASKS AND TASKS — one release for the array and every record it names, exactly
       as the three queues above. That they are gone at all is asserted at the top of this function; this is
       the release, not the check. */
    JS_FreeValue(ctx, f->jobs); f->jobs = JS_UNDEFINED;
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
        int had_frame = rf->frame != NULL, had_park = rf->parked != NULL;
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
       makes acct_compress_dead's re-pointing checkable rather than argued — a node it re-pointed and failed to
       release holds the whole ancestry above it, and a node is not a GC object, so nothing else would ever
       name the owner. It is also the only thing that would catch a family ROOT kept alive by a `family` tag
       nobody dropped: a departing flow clears its own (acct_depart), so a survivor here names the exit that
       did not. */
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
    /* …AND THE DELIVERY QUEUE, the same sentence a third time: no peer posts to most documents, and an Array
       per flow would put a JSObject on the heap for every member of a frontier that reached 29550 here. */
    f->deliver_q = JS_UNDEFINED;
    /* …AND WHICH SENDING TIMELINES THIS ONE IS IN, empty because a flow that has received nothing has
       committed to nothing — which is a POSITIVE statement and not a hole: with no commitment recorded, every
       sender's world is still one this timeline may be in. */
    f->deliver_world_q = JS_UNDEFINED;
    /* …AND THE JOB QUEUE, the same sentence a fourth time: most flows are enqueued nothing, and an Array per
       flow would put a JSObject on the heap for every member of a frontier that reached 29550 here. */
    f->jobs = JS_UNDEFINED;
    /* …and the register's own context, named HERE because this is the one point every flow passes through.
       Putting it in each host's setup would be the hand-copied list build.mjs warns about. */
    pending_set_ctx(ctx);
    f->val = 0.0; f->cpu = 0; f->visits = 0;
    /* …AND ITS PLACE IN THE FORK TREE, minted HERE because this is the one point every flow passes through, so
       no flow can exist without a node for its arms to be charged to. A from-baseline flow's node is a ROOT
       (`up` NULL); flow_fork_inherit is the only thing that ever attaches one under another. */
    f->acct = reclaim_calloc(1, sizeof *f->acct);
    CHECK(f->acct, "flow_add: OOM allocating a flow's fork-tree node — a flow whose arms cannot be charged back "
                   "lets a fork chain burn the thread forever without its rank ever moving");
    f->acct->owner = f; f->acct->refcount = 1;
    /* …AND IT IS ITS OWN FAMILY UNTIL SOMETHING SAYS OTHERWISE. A from-baseline flow founds a family and keeps
       this; a FORK joins its parent's (flow_fork_inherit) and this root becomes a plain ancestry node. Pointed
       here rather than left NULL so no flow can exist without an account for the aging term to read, which is
       the same reason the node above is minted here. */
    f->family = f->acct;
    g_acct_live++;
    f->cand_src = NULL; f->cand_payload = NULL; f->cand_sink = NULL;
    f->last_compiled = -1;   /* nothing compiled yet; see the no-replay DCHECK at the compile site */
    f->world = w;
    g_flows[g_flows_n++] = f;
    frontier_rank_changed();   /* frontier changed: the newcomer may outrank the flow holding the thread */
    return f;
}

/* THE QUEUE COORDINATE, forward-declared because the arrival rule below is the OTHER half of the split that
   defines it and the ordering itself is stated once, further down beside the terms it is made of. */
static double flow_queue_weight(const Flow *f);

/* THE ARRIVAL RULE — START-TIME FAIR QUEUEING'S `max{v(t), F_prev}`, APPLIED TO THE ENTRIES THAT WERE MISSING IT.
 *
 * flow_fork_inherit already states this rule and already obeys it: "a continuation of an active flow enters at
 * that flow's virtual time, never at the system's, or any flow can reset its own virtual clock by splitting."
 * That sentence is about a FORK because a fork was the only entry anybody had looked at. It is a statement about
 * ENTERING, and the other three ways a flow enters this frontier were all entering at virtual time ZERO:
 *
 *   - engine_join_document's boot flow (engine.c) — a Document the browser handed this agent mid-run,
 *   - solve.c's candidate session — one per @S search the run seeds, and
 *   - cold.c's park_flow_add — one per recipe the tier rebuilds.
 *
 * A flow at `cpu == 0` carries the FULL optimism bonus, so its weight is its reward + 1.0, and 1.0 is strictly
 * above every flow that has consumed a single quantum (bonus <= 0.5 against 0.012 of aging). So each of those
 * three minted a member at the very front of the queue, ahead of the entire backlog, no matter how long that
 * backlog had been waiting — which is the exact defect flow_fork_inherit was written to close, arriving through
 * the doors it did not cover. It is worst at the first of them, because that one is reachable from the PAGE: a
 * document that creates same-origin navigables mints a boot flow per document, so a page can promote work it
 * MANUFACTURES above every flow already on the frontier, without the manufacturing flow paying anything for it.
 * Inheritance closes that for `f(){g()}` and left it open for `open()`.
 *
 * v(t) IS THE SERVICE OF THE FLOW IN SERVICE, which is SFQ's own definition and is O(1) — not a minimum over the
 * frontier, which would be O(n) inside a function the pick already calls n times. When nothing is running there
 * is no busy period to enter and v(0) is 0, which is why the first flow of a session, and every recipe the cold
 * tier rebuilds before the first step, still enter at 0 exactly as they did.
 *
 * IT CANNOT INVERT A PAIR THAT ALREADY EXISTS. Every weight in the frontier is untouched; the only thing that
 * changes is where a NEW member is placed among them, so no existing ordering decision can be reversed by this.
 *
 * IT IS THREE TAGS NOW AND IT WAS ONE, WHICH IS THE HALF THE FAMILY CHARGE MADE VISIBLE. `cpu` used to carry both
 * of flow_weight's non-reward terms, so copying it placed the newcomer in both dimensions at once. With the
 * aging reading the FAMILY (FlowAcct's `fam_us`), a from-baseline flow founds a family whose service is ZERO —
 * so copying `cpu` alone would hand it the incumbent's spent optimism and none of the incumbent's aging, and it
 * would arrive STRICTLY ABOVE the flow in service the moment that flow's family had burned more than its reward
 * is worth. Which is not a hypothetical: it is the ordinary state of this frontier (svcFamMax 8910 against a
 * valMax of 10.0), so the newcomer would jump the entire backlog exactly as it did before this rule existed.
 * The assertion below is what says so — it FIRES on the one-tag version of this function — and the fix is that
 * the rule assigns every term the weight is made of, which is what "enters at the system's virtual time" means
 * once the system's virtual time has two coordinates.
 *
 * IT IS FOUR TAGS NOW AND THE FOURTH IS THE REWARD, WHICH IS THE ONE THAT DECIDES ANYTHING. The sentence above
 * — "the rule assigns every term the weight is made of" — was written while the rule assigned THREE of four,
 * and the term it left out is the term the whole ordering is built on. A one-sided `<=` cannot see that: the
 * omission can only push a newcomer DOWN, and the assert passed however far down it went. flow_fork_inherit's
 * parallel assertion is an EQUALITY over the weight and would have fired at the very first fork, which is
 * exactly the difference between an assert that FORCES a rule and one that merely records a direction.
 *
 * WHAT IT COST, MEASURED, AND IT IS THE WHOLE @S SURFACE. `val` is copied to every arm at every fork, so a
 * document's frontier stands at its boot family's reward — 401 members at `valMax` 118 on the smoke fixture —
 * while a from-baseline flow entered at reward ZERO. The only members of that frontier at zero were the twelve
 * @S candidate sessions (`valZero:12` beside `cands:12`, the same twelve flows counted twice), so their weight
 * was `1.0 − aging` = 0.976 for the entire run against a `wTop` that never fell below 56. They were never
 * picked: `turns:0` on all twelve after 254,181 switches, in the field solve.h added precisely to tell "the WFQ
 * has never once given this search the thread" from "its flows have run and have not reached the sink". Every
 * §@S mechanism — the delivery probe, the derived breakout, the byte-provenance distance, the fire — executes
 * only inside a candidate flow, so on any document whose exploration keeps emitting, all of them were
 * unreachable BY CONSTRUCTION and the run reported a clean parked search instead of a starved one.
 *
 * INHERITING THE REWARD IS NOT CREDITING IT, and the design already carries the field that says so. `val_born`
 * is "how much of `val` this flow did not earn"; a fork takes its parent's reward and marks all of it
 * inherited, and a newcomer at v(t) is the same statement made at the other door. It cannot promote either: the
 * newcomer TIES with the flow in service, and flow_pick's STRICT comparison leaves the thread where it is until
 * the incumbent's own service moves it — which is start-time fair queueing doing exactly what this rule is.
 * The prose above already reached this conclusion for the boot-flow door and stopped one step short of it:
 * "Inheritance closes that for `f(){g()}` and left it open for `open()`." Inheritance is the closure; it was
 * applied to three coordinates and not to the fourth. */
static void flow_arrive_at_virtual_time(Flow *f) {
    DCHECK(f->val == 0.0 && f->val_born == 0.0 && f->cpu == 0 && f->visits == 0 &&
           f->family == f->acct && f->family->fam_us == 0,
           "a from-baseline flow was charged or handed an account before it arrived — the arrival rule ASSIGNS "
           "every term, so a caller that wrote one first has ranked this flow at a virtual time nobody chose "
           "and this assignment throws that away");
    if (!g_running) return;   /* SFQ's v(0): no busy period to enter, so the frontier's clock is still at zero */
    DCHECK(flow_is_member(g_running),
           "the flow holding the thread is not a member of the frontier, so the virtual time a newcomer would "
           "enter at is a tag belonging to no queue — a newcomer placed against it is placed against nothing");
    f->val = g_running->val;                              /* the reward term's coordinate */
    /* …AND THE MARK THAT NONE OF IT IS THIS FLOW'S OWN, written beside the copy for the same reason
       flow_fork_inherit writes it beside its own: `val` is now a statement about an ANCESTRY, and the census
       row that asks what a member has emitted ITSELF (`val > val_born`) would otherwise read a whole frontier
       of newcomers as productive on the strength of the flow that happened to be running when they arrived. */
    f->val_born = g_running->val;
    f->visits = g_running->visits;                        /* the optimism term's coordinate */
    f->cpu = g_running->cpu;                              /* …and the aging term's own half */
    f->family->fam_us = acct_family_us(g_running);        /* …and its family half */
    /* THE RULE ITSELF, ASSERTED WHERE IT IS APPLIED, AND IT IS AN EQUALITY. A newcomer enters AT the system's
       virtual time — not at or below it — and flow_pick's STRICT comparison is what then leaves the thread with
       the incumbent, so a tie is the whole of what "arrives" means and there is nothing left for a `<=` to
       express. Written over flow_queue_weight rather than over a list of fields for the reason
       flow_fork_inherit gives about its own: a list cannot fire when a term is ADDED, and every term this rule
       has ever been missing was added to the weight by somebody who did not know this function existed.
       THE FITNESS IS THE ONE TERM NAMED RATHER THAN CARRIED, and naming it is what keeps this exact. §@S's
       distance is a fraction OF A PAYLOAD, and a newcomer's payload is not the payload the flow in service
       holds — there is no position for it to arrive at, which is why flow_queue_weight excludes it and why a
       second per-item reading added to flow_weight would fire flow_fork_inherit's assert instead of this one.
       EXACT WITH NO EPSILON: all four tags are copied verbatim and the silence notch is an integer division of
       copied integers, so the two sides are the same float expression over the same values. */
    DCHECK(flow_queue_weight(f) == flow_queue_weight(g_running),
           "a flow arriving from the baseline did not enter at the frontier's virtual time — it was placed "
           "above the flow in service (a page promoting the documents and candidate sessions it creates over "
           "the whole backlog) or below it (a newcomer the ordering can never reach, which is what starved "
           "every @S candidate session behind a boot family whose reward its arms had all inherited)");
}

/* THE PROGRAMS A NEW FLOW STARTS WITH — installed by the session, asked at the ONE place a flow is created.
   The scheduler may not know what a document's programs ARE (that is the engine's inventory, and this layer
   owns the registry), so the answer arrives as a hook exactly as the timer, rendering and checkpoint steps do.
   IT IS ASKED HERE AND NOT AT EACH CREATOR because a flow with no sequence runs NOTHING and says nothing about
   it: a creator that forgot would add a member that finishes immediately, which is indistinguishable from a
   document whose scripts all completed. There are four creators of a fresh timeline of this agent's root
   document — the boot flow, a cold-resumed flow replaying from its first script, an @S candidate session, and
   whatever is written next — and a hand-copied seeding line at each is the same hand-picked list this project
   forbids for per-realm intrinsics, for the same reason: the one that is missed is silent. */
static void (*g_seed_hook)(Flow *f);
void flow_set_seed_hook(void (*fn)(Flow *f)) {
    DCHECK(fn == NULL || g_seed_hook == NULL,
           "a second component claimed the flow-seeding step — a new flow's program sequence has one source, "
           "and the second claim would silently replace the first for every flow created after it");
    g_seed_hook = fn;
}

/* A FLOW WHOSE SEQUENCE ITS CREATOR ESTABLISHES ITSELF — the fork (which INHERITS the parent's rows) and a
   joined document's boot flow (which is seeded with THAT document's programs, not the root's). Both would be
   wrong with the root document's sequence on them: the fork would leak the rows its own copy then overwrites,
   and the joined flow would re-run the root's bundle in a second timeline. Named rather than defaulted, so the
   two exceptions are the two that say so and everything else gets the sequence it needs. */
Flow *flow_add_unseeded(JSContext *ctx, JSValueConst fn, WorldId parent) {
    /* THE WORLD IS MINTED HERE so no flow can exist without one. A fork passes the world it BRANCHED AT and the
       child records the edge, which is what lets another instance materialize this flow's segment by forking
       the nearest ancestor it already holds. A from-baseline flow passes WORLD_NONE and gets a root.
       `parent` IS THE FORK POINT AND NOT THE OTHER ARM'S WORLD: the arm that keeps running is re-minted as a
       child of the same name (engine_sibling_assemble), so the branch retires its own world and the two arms
       are siblings rather than ancestor-and-descendant — which is what makes "do these two senders contradict?"
       answerable at a peer. */
    int from_baseline = world_is_none(parent);
    Flow *f = flow_new(ctx, fn, from_baseline ? world_mint() : world_mint_child(parent));
    /* AND WHERE IT ENTERS THE QUEUE, decided by the SAME predicate that decides where it enters the world tree,
       because they are one question asked twice: a flow standing on another flow's decisions is a continuation
       and takes that flow's account (flow_fork_inherit, at the fork), and a flow standing on nobody's arrives
       at the frontier's virtual time. `parent` is the only thing that tells the two apart, so the arrival is
       decided here rather than at each of the three from-baseline call sites, none of which knew it was making
       a ranking decision at all. */
    if (from_baseline) flow_arrive_at_virtual_time(f);
    return f;
}

Flow *flow_add(JSContext *ctx, JSValueConst fn, WorldId parent) {
    Flow *f = flow_add_unseeded(ctx, fn, parent);
    /* THE SEQUENCE IS ESTABLISHED BEFORE THE FLOW IS EVER PICKED, which is what makes the seeding part of
       CREATING a flow rather than a step it takes. A member the scheduler could pick with an empty sequence
       would report itself finished, and "finished" is the one answer that is never retried. */
    DCHECK(g_seed_hook != NULL,
           "a flow was added to the frontier with nothing to seed its program sequence — the session installs "
           "that hook when it opens, so this flow was created outside a session and would run no program at "
           "all and then report itself finished");
    g_seed_hook(f);
    return f;
}

/* BLOCKED = holding an unanswered synchronous host request. Scanned rather than counted because a counter is
   a second representation of the same fact, and the two drift at exactly the sites (fork, drain, release) that
   are already the hardest to keep in step. The register is short — it is one flow's outstanding requests. */
int flow_blocked(const Flow *f) { return pending_blocked(f->pending); }

/* THE DELIVERY QUEUE'S LENGTH — see flow.h. Same context and same tag assert as the operation queue below it,
   because it is the same shape: an Array this engine appends to and nothing outside it touches. */
int flow_deliver_pending(const Flow *f) {
    JSValue v;
    int n;

    if (!JS_IsObject(f->deliver_q)) return 0;
    v = JS_GetPropertyStr(pending_ctx(), f->deliver_q, "length");
    DCHECK(JS_VALUE_GET_TAG(v) == JS_TAG_INT,
           "a flow's delivery queue has no small-integer length — it is the engine's own Array and nothing "
           "outside it appends");
    n = JS_VALUE_GET_INT(v);
    JS_FreeValue(pending_ctx(), v);
    return n;
}

/* THE UNMADE-DELIVERY FIFO ITSELF — see flow.h for the shape and for why all three mutators live beside the
   field. An entry is an immutable two-element [record, senderOrigin] Array; nothing edits one after the push,
   so append / take-from-the-front / fork is the whole of it. */
void flow_deliver_push(JSContext *ctx, Flow *f, const char *record, const char *sender_origin) {
    JSValue e;

    DCHECK(record != NULL && *record, "a routed delivery was queued with no record");
    DCHECK(sender_origin != NULL && *sender_origin,
           "a routed delivery was queued with no SENDER ORIGIN — only the trusted zone may stamp one, and it "
           "is the field every `event.origin` check in every bundle is written against");
    e = JS_NewArray(ctx);
    CHECK(!JS_IsException(e), "flow: OOM allocating a routed delivery — a dropped record is a message a peer "
                              "sent and this document never received, which the page cannot tell from having "
                              "registered no handler");
    cow_engine_write_begin();
    JS_SetPropertyUint32(ctx, e, 0, JS_NewString(ctx, record));
    JS_SetPropertyUint32(ctx, e, 1, JS_NewString(ctx, sender_origin));
    if (!JS_IsObject(f->deliver_q)) {
        JS_FreeValue(ctx, f->deliver_q);
        f->deliver_q = JS_NewArray(ctx);
        CHECK(!JS_IsException(f->deliver_q), "flow: OOM allocating a flow's delivery queue");
    }
    JS_SetPropertyUint32(ctx, f->deliver_q, (uint32_t)flow_deliver_pending(f), e);
    cow_engine_write_end();
}

/* TAKE THE OLDEST, which is the whole of why this is a queue and not a set: HTML §9.3.3 "Posting messages"
   ends the window post message steps by queueing a global task on the posted message task source, and
   §8.1.7.1 "Definitions" gives a task a source in order to "group and serialize related tasks" — so the order
   two messages arrived in is the order the page must observe them in. A swap-remove would hand the page one
   sender's second message before its first. */
JSValue flow_deliver_take(JSContext *ctx, Flow *f) {
    int n = flow_deliver_pending(f), i;
    JSValue e;

    DCHECK(n > 0, "a delivery was taken from a flow whose queue is empty — the caller tested the queue to get "
                  "here, so the two reads have come apart");
    e = flow_deliver_entry(f, 0);
    cow_engine_write_begin();
    for (i = 1; i < n; i++)
        JS_SetPropertyUint32(ctx, f->deliver_q, (uint32_t)(i - 1),
                             JS_GetPropertyUint32(ctx, f->deliver_q, (uint32_t)i));
    JS_SetPropertyStr(ctx, f->deliver_q, "length", JS_NewInt32(ctx, n - 1));
    cow_engine_write_end();
    return e;
}

/* ONE ENTRY, WITHOUT CONSUMING IT — the accessor both non-consuming readers go through (engine_route compares
   the arriving record's world against every queued one, the cold tier writes them out), so the pair's shape is
   asserted in ONE place however many callers read it. */
JSValue flow_deliver_entry(const Flow *f, int i) {
    JSValue e;

    DCHECK(i >= 0 && i < flow_deliver_pending(f),
           "a flow's delivery queue was read past its end — the caller asked it for its length to get here");
    e = JS_GetPropertyUint32(pending_ctx(), f->deliver_q, (uint32_t)i);
    DCHECK(JS_IsObject(e), "a flow's delivery queue held something that is not a [record, senderOrigin] pair — "
                           "it is this engine's own Array and nothing outside flow_deliver_push appends");
    return e;
}

/* THE ARM'S OWN QUEUE — a new Array naming the parent's ENTRIES. The arm is that timeline continued, so a
   message that arrived in this document before the branch arrived in the arm too, and it makes its own
   delivery under its own delta: the multiplicity §7.2.1 has when a document's state is its flows, not one
   message delivered twice. The ARRAY is per-flow because each arm consumes at its own rate; the ENTRIES are
   shared because none of them ever changes. An empty queue forks as JS_UNDEFINED — the common case stays a
   tag test, and flow_deliver_push mints the Array again if the arm is ever routed one. */
JSValue flow_deliver_fork(JSContext *ctx, const Flow *parent) {
    int n = flow_deliver_pending(parent), i;
    JSValue out;

    if (n == 0) return JS_UNDEFINED;
    out = JS_NewArray(ctx);
    CHECK(!JS_IsException(out), "flow: OOM forking a flow's delivery queue — an arm that lost it is a timeline "
                                "the peer's message never reached");
    cow_engine_write_begin();
    for (i = 0; i < n; i++)
        JS_SetPropertyUint32(ctx, out, (uint32_t)i, JS_GetPropertyUint32(ctx, parent->deliver_q, (uint32_t)i));
    cow_engine_write_end();
    return out;
}

/* ─── WHICH SENDING TIMELINES THIS ONE IS IN (flow.h's `deliver_world_q`) ───────────────────────────────
 * The same four operations as the queue above, over the same [a, b] pair shape, and separate from it for the
 * reason flow.h gives: the queue is work this timeline still owes, this is what it has already become. An
 * entry is never edited after it is pushed, so there is nothing here but append, read, count and fork. */
int flow_world_commits(const Flow *f) {
    JSValue v;
    int n;

    if (!JS_IsObject(f->deliver_world_q)) return 0;
    v = JS_GetPropertyStr(pending_ctx(), f->deliver_world_q, "length");
    DCHECK(JS_VALUE_GET_TAG(v) == JS_TAG_INT,
           "a flow's delivery-world record has no small-integer length — it is the engine's own Array and "
           "nothing outside flow_world_commit_push appends");
    n = JS_VALUE_GET_INT(v);
    JS_FreeValue(pending_ctx(), v);
    return n;
}

JSValue flow_world_commit_at(const Flow *f, int i) {
    JSValue e;

    DCHECK(i >= 0 && i < flow_world_commits(f),
           "a flow's delivery-world record was read past its end — the caller asked it for its length to get "
           "here");
    e = JS_GetPropertyUint32(pending_ctx(), f->deliver_world_q, (uint32_t)i);
    DCHECK(JS_IsObject(e), "a flow's delivery-world record held something that is not a [vector, taken] pair — "
                           "it is this engine's own Array and nothing outside flow_world_commit_push appends");
    return e;
}

/* APPEND ONE. Inside cow_engine_write_begin/end for the reason the queue beside it is: this is the SCHEDULER's
   record about a flow, written from outside any flow's delta, and a delta that captured it would un-commit a
   timeline the moment a sibling switched in — which is exactly the state this field exists to make
   impossible. */
void flow_world_commit_push(JSContext *ctx, Flow *f, const char *vector, int taken) {
    JSValue e;

    DCHECK(vector != NULL && *vector,
           "a receiving timeline recorded a commitment to no world — the record decides which of a sender's "
           "arms this timeline may still hear from, and an empty one constrains nothing at all");
    DCHECK(taken == 0 || taken == 1,
           "a delivery-world commitment carried something other than RECEIVED or FORECLOSED — the two are the "
           "whole vocabulary, and a third value would be read as one of them by whichever test asked first");
    e = JS_NewArray(ctx);
    CHECK(!JS_IsException(e), "flow: OOM recording which sending timeline a receiving flow is in — without it "
                              "this timeline would go on to accept a message from the arm it did not take");
    cow_engine_write_begin();
    JS_SetPropertyUint32(ctx, e, 0, JS_NewString(ctx, vector));
    JS_SetPropertyUint32(ctx, e, 1, JS_NewInt32(ctx, taken));
    if (!JS_IsObject(f->deliver_world_q)) {
        JS_FreeValue(ctx, f->deliver_world_q);
        f->deliver_world_q = JS_NewArray(ctx);
        CHECK(!JS_IsException(f->deliver_world_q), "flow: OOM allocating a flow's delivery-world record");
    }
    JS_SetPropertyUint32(ctx, f->deliver_world_q, (uint32_t)flow_world_commits(f), e);
    cow_engine_write_end();
}

/* THE ARM'S OWN RECORD — a new Array naming the parent's entries, and then the fork adds the one entry that
   makes the two arms different timelines. It is the same split as every other per-flow queue here (the ARRAY
   is per-flow because each arm commits on its own from the branch onward; the ENTRIES are shared because none
   of them ever changes), and it is why the field is carried at all: an arm that lost its parent's commitments
   would re-accept a message its own timeline had already foreclosed. */
JSValue flow_world_commit_fork(JSContext *ctx, const Flow *parent) {
    int n = flow_world_commits(parent), i;
    JSValue out;

    if (n == 0) return JS_UNDEFINED;
    out = JS_NewArray(ctx);
    CHECK(!JS_IsException(out), "flow: OOM forking which sending timelines a flow is in — an arm without them "
                                "is a receiver that has forgotten which of its sender's arms it took");
    cow_engine_write_begin();
    for (i = 0; i < n; i++)
        JS_SetPropertyUint32(ctx, out, (uint32_t)i,
                             JS_GetPropertyUint32(ctx, parent->deliver_world_q, (uint32_t)i));
    cow_engine_write_end();
    return out;
}

/* ───────────────────────────── THE JOB QUEUE ─────────────────────────────
 * See flow.h for what a record is and why each part of it is there. Everything that ever happens to the queue
 * is in this block, so the Array's shape has one reader.
 *
 * THE CALLEE TABLE. quickjs's job functions are statics of the interpreter and the host sees them only as
 * pointers arriving at the enqueue hook, so a name for one can only be minted on FIRST SIGHT. Four of them
 * exist in this program (promise_reaction_job, js_promise_resolve_thenable_job, js_dynamic_import_job, and
 * host_call_job for every platform edge); the table is generous and CRASHES rather than growing, because a
 * fifth arriving is a fact about the interpreter that should be read by a person and not absorbed. A code
 * address is stable for the process, so the table needs no teardown — and it is deliberately NOT reset with
 * the frontier: an ordinal handed out in one session must not be re-used for a different callee in the next
 * while any record still names it. */
#define FLOW_JOB_FN_MAX 16
static JSJobFunc *g_job_fn[FLOW_JOB_FN_MAX];
static int g_job_fn_n;

static int job_fn_id(JSJobFunc *fn) {
    int i;

    DCHECK(fn != NULL, "a job was queued with no callee — there is nothing for the pick to run and the record "
                       "would be a work item that can never be performed");
    for (i = 0; i < g_job_fn_n; i++)
        if (g_job_fn[i] == fn) return i;
    CHECK(g_job_fn_n < FLOW_JOB_FN_MAX,
          "flow: more distinct job callees than this engine knows how to name — the table is what keeps a "
          "raw function pointer out of the queue's records, so a new one is an interpreter edge to read");
    g_job_fn[g_job_fn_n] = fn;
    return g_job_fn_n++;
}

/* THE RECORD'S LAYOUT, stated once. The header is fixed-width and the arguments follow it, so `argc` is a
   length subtraction and never a stored count that can disagree with what is there. */
enum { JOB_FN = 0, JOB_TASK, JOB_EXTERNAL, JOB_GLOBAL, JOB_HANDLE, JOB_HDR };

/* THE PROVENANCE BRACKET — see flow.h. A static rather than a field on the flow because it is a property of
   the CONVERSION in flight, not of the timeline: exactly one can be open, which is what the asserts say. */
static int g_job_external;

void flow_job_external_begin(void) {
    DCHECK(!g_job_external, "a second routed record was being turned into local work while the first was still "
                            "in flight — a delivery is made from flow_step with no frame, so two cannot "
                            "overlap, and a nested bracket would mark the wrong jobs external");
    g_job_external = 1;
}

void flow_job_external_end(void) {
    DCHECK(g_job_external, "the external-work bracket was closed without being opened — the pair is what says "
                           "which jobs a replay would not re-cause");
    g_job_external = 0;
}

int flow_job_pending(const Flow *f) {
    JSValue v;
    int n;

    if (!JS_IsObject(f->jobs)) return 0;
    v = JS_GetPropertyStr(pending_ctx(), f->jobs, "length");
    DCHECK(JS_VALUE_GET_TAG(v) == JS_TAG_INT,
           "a flow's job queue has no small-integer length — it is the engine's own Array and nothing outside "
           "flow_job_push appends");
    n = JS_VALUE_GET_INT(v);
    JS_FreeValue(pending_ctx(), v);
    return n;
}

/* ONE ENTRY, BORROWED — the accessor every reader in this block goes through, so the record's shape is
   asserted in one place however many of them there are. The caller OWNS the reference it gets back. */
static JSValue job_entry(const Flow *f, int i) {
    JSValue e = JS_GetPropertyUint32(pending_ctx(), f->jobs, (uint32_t)i);

    DCHECK(JS_IsObject(e), "a flow's job queue held something that is not a job record — it is this engine's "
                           "own Array and nothing outside flow_job_push appends");
    return e;
}

static int job_field_int(JSValueConst e, int field) {
    JSValue v = JS_GetPropertyUint32(pending_ctx(), e, (uint32_t)field);
    int n;

    DCHECK(JS_VALUE_GET_TAG(v) == JS_TAG_INT,
           "a job record's header field is not a small integer — the header is written whole at the push, so "
           "one that is not there is a record something other than this file built");
    n = JS_VALUE_GET_INT(v);
    JS_FreeValue(pending_ctx(), v);
    return n;
}

/* THE HANDLE FIELD, READ BACK — a SECOND accessor rather than a widened job_field_int, because the two fields
   have different shapes and the assert is the point: a header int is a small integer and must be one, while a
   handle is an integer VALUE that leaves the small-integer tag behind at 2^31 and is still exact. Both tags are
   accepted and integrality is what is asserted, so a handle that failed to round-trip crashes here rather than
   naming a different queued task at the removal. That is the same silent wrong answer js_task_handle_new's
   ceiling assert refuses at the issuing end, asserted again at this end, which is the one that USES it. */
static JSTaskHandle job_field_handle(JSValueConst e, int field) {
    JSValue v = JS_GetPropertyUint32(pending_ctx(), e, (uint32_t)field);
    JSTaskHandle h;

    if (JS_VALUE_GET_TAG(v) == JS_TAG_INT) {
        int n = JS_VALUE_GET_INT(v);

        DCHECK(n >= 0, "a job record's handle is negative — handles come from one monotone runtime counter "
                       "that starts at 1, so this record was written by something other than flow_job_push");
        h = (JSTaskHandle)n;
    } else {
        double d;

        DCHECK(JS_TAG_IS_FLOAT64(JS_VALUE_GET_TAG(v)),
               "a job record's handle is not a number — the header is written whole at the push, so one that "
               "is not there is a record something other than this file built");
        d = JS_VALUE_GET_FLOAT64(v);
        DCHECK(d >= 0 && d < 9007199254740992.0 && d == (double)(uint64_t)d,
               "a job record's handle is not an exact integer below 2^53 — a handle names at most one queued "
               "callback and that is only true while the number round-trips, so a removal made with this one "
               "would take some other timeline's task off the queue instead of finding none");
        h = (JSTaskHandle)d;
    }
    JS_FreeValue(pending_ctx(), v);
    return h;
}

int flow_job_microtask(const Flow *f) {
    int n = flow_job_pending(f), i;

    for (i = 0; i < n; i++) {
        JSValue e = job_entry(f, i);
        int task = job_field_int(e, JOB_TASK);

        JS_FreeValue(pending_ctx(), e);
        if (!task) return 1;
    }
    return 0;
}

void flow_job_push(JSContext *ctx, Flow *f, JSJobFunc *fn, int argc, JSValueConst *argv, int task,
                   JSTaskHandle handle) {
    JSValue e;
    int i;

    DCHECK(argc >= 0, "a job was queued with a negative argument count");
    DCHECK(argc == 0 || argv != NULL, "a job was queued claiming arguments it did not bring");
    e = JS_NewArray(ctx);
    CHECK(!JS_IsException(e),
          "flow: OOM allocating a queued job — a dropped reaction corrupts async exploration, and it is "
          "invisible from outside: it looks exactly like a page that registered no handler");
    cow_engine_write_begin();
    JS_SetPropertyUint32(ctx, e, JOB_FN, JS_NewInt32(ctx, job_fn_id(fn)));
    JS_SetPropertyUint32(ctx, e, JOB_TASK, JS_NewInt32(ctx, task ? 1 : 0));
    JS_SetPropertyUint32(ctx, e, JOB_EXTERNAL, JS_NewInt32(ctx, g_job_external));
    /* THE KEY §7.5.10 "Destroying documents"'s step 7 COMPARES, held as a reference — see flow.h.
       JS_GetGlobalObject is the realm's identity and the one thing about it that is a JS value; a destroyed
       document's jobs are then found by comparing two objects rather than two pointers, one of which used to
       be able to be freed underneath the comparison. */
    JS_SetPropertyUint32(ctx, e, JOB_GLOBAL, JS_GetGlobalObject(ctx));
    /* THE NAME THE RUNTIME ISSUED — see flow.h's HANDLE bullet for why it is a JS number and not a string or a
       word pair. JS_NewInt64 keeps the small-integer tag for the first 2^31 handles and widens to a float64
       past it; both are exact below 2^53, which js_task_handle_new asserts is the whole range. */
    JS_SetPropertyUint32(ctx, e, JOB_HANDLE, JS_NewInt64(ctx, (int64_t)handle));
    for (i = 0; i < argc; i++)
        JS_SetPropertyUint32(ctx, e, (uint32_t)(JOB_HDR + i), JS_DupValue(ctx, argv[i]));
    if (!JS_IsObject(f->jobs)) {
        JS_FreeValue(ctx, f->jobs);
        f->jobs = JS_NewArray(ctx);
        CHECK(!JS_IsException(f->jobs), "flow: OOM allocating a flow's job queue");
    }
    JS_SetPropertyUint32(ctx, f->jobs, (uint32_t)flow_job_pending(f), e);
    cow_engine_write_end();
}

JSValue flow_job_take(JSContext *ctx, Flow *f) {
    int n = flow_job_pending(f), pick = 0, i;
    JSValue e;

    DCHECK(n > 0, "a job was taken from a flow whose queue is empty — the caller tested the queue to get here, "
                  "so the two reads have come apart");
    /* THE CHECKPOINT RULE, and it is the whole reason this is not a FIFO pop. Within a queue the order is
       arrival order; ACROSS the two, a task may not begin while this flow still holds a microtask. */
    while (pick < n) {
        JSValue c = job_entry(f, pick);
        int task = job_field_int(c, JOB_TASK);

        JS_FreeValue(ctx, c);
        if (!task) break;
        pick++;
    }
    if (pick == n) pick = 0;   /* nothing but tasks: the checkpoint is done, run the earliest task */
    e = job_entry(f, pick);
    cow_engine_write_begin();
    for (i = pick + 1; i < n; i++)
        JS_SetPropertyUint32(ctx, f->jobs, (uint32_t)(i - 1), JS_GetPropertyUint32(ctx, f->jobs, (uint32_t)i));
    JS_SetPropertyStr(ctx, f->jobs, "length", JS_NewInt32(ctx, n - 1));
    cow_engine_write_end();
    return e;
}

JSValue flow_job_run(JSContext *ctx, JSValueConst entry) {
    JSValue stack[8], *args = stack, r;
    JSValue len = JS_GetPropertyStr(ctx, entry, "length");
    int argc, i, id;

    DCHECK(JS_VALUE_GET_TAG(len) == JS_TAG_INT, "a job record has no small-integer length");
    argc = JS_VALUE_GET_INT(len) - JOB_HDR;
    JS_FreeValue(ctx, len);
    DCHECK(argc >= 0, "a job record is shorter than its own header — the header is written whole at the push");
    id = job_field_int(entry, JOB_FN);
    DCHECK(id >= 0 && id < g_job_fn_n,
           "a job record names a callee this session never saw — the table is append-only and an ordinal is "
           "minted at the push, so an ordinal past its end is a record from another session");
    if (argc > (int)(sizeof stack / sizeof *stack)) {
        args = malloc(sizeof(*args) * (size_t)argc);
        CHECK(args != NULL, "flow: OOM building a queued job's argument vector — the job is on the frontier "
                            "and the WFQ may never drop a work item");
    }
    for (i = 0; i < argc; i++)
        args[i] = JS_GetPropertyUint32(ctx, entry, (uint32_t)(JOB_HDR + i));
    r = g_job_fn[id](ctx, argc, (JSValueConst *)args);
    for (i = 0; i < argc; i++)
        JS_FreeValue(ctx, args[i]);
    if (args != stack) free(args);
    return r;
}

int flow_job_drop_realm(JSContext *ctx, Flow *f, JSContext *realm) {
    int n = flow_job_pending(f), keep = 0, i;
    JSValue global;

    if (n == 0) return 0;
    DCHECK(realm != NULL, "destroy a document step 7 was asked to remove the tasks of no realm at all");
    global = JS_GetGlobalObject(realm);
    cow_engine_write_begin();
    /* COMPACTED IN ONE FORWARD PASS rather than spliced per hit: the walk that used this queue removed from
       the middle with a memmove per removal, which is the same answer at N times the cost and one more index
       to get wrong. Survivors keep their arrival order, which is what the task source guarantees the page. */
    for (i = 0; i < n; i++) {
        JSValue e = job_entry(f, i);
        JSValue g = JS_GetPropertyUint32(ctx, e, JOB_GLOBAL);
        int mine;

        DCHECK(JS_IsObject(g), "a job record carries no enqueuing realm — §7.5.10 step 7 keys on it, so a "
                               "record without one can neither be dropped with its document nor safely left "
                               "queued");
        mine = JS_VALUE_GET_PTR(g) == JS_VALUE_GET_PTR(global);

        JS_FreeValue(ctx, g);
        if (mine) { JS_FreeValue(ctx, e); continue; }
        JS_SetPropertyUint32(ctx, f->jobs, (uint32_t)keep++, e);
    }
    JS_SetPropertyStr(ctx, f->jobs, "length", JS_NewInt32(ctx, keep));
    cow_engine_write_end();
    JS_FreeValue(ctx, global);
    return n - keep;
}

/* REMOVE ONE JOB BY NAME — see flow.h. The same forward-compacting pass as flow_job_drop_realm with the other
   predicate, deliberately not folded into it behind a mode flag: the two questions have different answers about
   how many entries may match ("every job of this document" versus "the one job called this"), and that
   difference is the assert below.
   THE CONTEXT IS THE QUEUE'S OWN. JSJobRemoveHook names a job by identity and by nothing else — there is no
   realm in the question, because a handle is issued by one runtime counter and is not a fact about a document —
   so the caller has none to hand down, and this is the same context every reader in this block already uses. */
int flow_job_remove(Flow *f, JSTaskHandle handle) {
    JSContext *ctx = pending_ctx();
    int n = flow_job_pending(f), keep = 0, found = 0, i;

    DCHECK(handle != JS_TASK_HANDLE_NONE,
           "a queued job was asked for by the never-issued handle — a record that never went through the "
           "runtime's enqueue carries exactly that, so this would match work no tracker ever named");
    if (n == 0) return 0;
    cow_engine_write_begin();
    for (i = 0; i < n; i++) {
        JSValue e = job_entry(f, i);

        /* EVERY entry is compared, including after a hit, because the count is what the assert is made of. */
        if (job_field_handle(e, JOB_HANDLE) == handle) {
            found++;
            JS_FreeValue(ctx, e);
            continue;
        }
        JS_SetPropertyUint32(ctx, f->jobs, (uint32_t)keep++, e);
    }
    JS_SetPropertyStr(ctx, f->jobs, "length", JS_NewInt32(ctx, keep));
    cow_engine_write_end();
    DCHECK(found <= 1,
           "one handle named two jobs on ONE flow's queue — handles come from a single monotone runtime "
           "counter and are never reused, and a fork gives an arm its own Array naming each record once, so "
           "this is two entries wearing one name and the tracker's removal just took the wrong task");
    return found;
}

JSValue flow_job_fork(JSContext *ctx, const Flow *parent) {
    int n = flow_job_pending(parent), i;
    JSValue out;

    /* THE ARM INHERITS THE QUEUE, and its absence was a real bug one layer down from the delivery queue's: an
       arm forked with a `.then` already attached silently never ran it, and it surfaced as a rejection
       reported unhandled in the arm whose `.catch` job never arrived. An empty queue forks as JS_UNDEFINED —
       the common case stays a tag test, and flow_job_push mints the Array again if the arm is ever enqueued. */
    if (n == 0) return JS_UNDEFINED;
    out = JS_NewArray(ctx);
    CHECK(!JS_IsException(out), "flow: OOM forking a flow's job queue — an arm that lost it is a timeline whose "
                                "reactions never run, which the WFQ is forbidden to do to a work item");
    cow_engine_write_begin();
    for (i = 0; i < n; i++)
        JS_SetPropertyUint32(ctx, out, (uint32_t)i, JS_GetPropertyUint32(ctx, parent->jobs, (uint32_t)i));
    cow_engine_write_end();
    return out;
}

int flow_job_external(const Flow *f) {
    int n = flow_job_pending(f), i, k = 0;

    for (i = 0; i < n; i++) {
        JSValue e = job_entry(f, i);

        k += job_field_int(e, JOB_EXTERNAL) ? 1 : 0;
        JS_FreeValue(pending_ctx(), e);
    }
    return k;
}

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
/* HOW MANY MICROSECONDS OF SILENCE ONE EMITTED FINDING IS WORTH — the PRICE, and nothing else. One second,
 * which is what the paragraph above argues for; FLOW_AGE_RATE is its reciprocal rather than a second number, so
 * the two cannot drift and there is one place to change the price.
 *
 * IT IS THE PRICE AND IT IS NOT THE UNIT, AND CONFLATING THE TWO IS WHAT THIS BLOCK NOW SEPARATES. A term of
 * an ORDER has two independent quantities in it — what a microsecond COSTS, and the smallest step the order
 * can express — and one constant cannot be both. The price has to be commensurate with an emitted finding
 * (that is the whole of the paragraph above). The step has to be commensurate with the granularity at which
 * the thread is actually handed over, because a term that steps coarser than that is INVISIBLE to every pick
 * inside one step of it: the scheduler charges the flow, re-picks, and reads a weight the charge did not move.
 *
 * WHAT THAT COST, MEASURED ON REAL PAGES. With one constant serving both, the step was one SECOND, which is
 * 83.3 cooperative quanta of SILENCE — and the flow being charged accrues silence at TWICE the rate it burns
 * CPU, because flow_age_running adds the same microseconds to `cpu` AND to the family's `fam_us` and the notch
 * is their SUM. So the running flow crossed a step once every 41.7 quanta it consumed: 40 of every 41 picks
 * were made against a weight the previous pick's service had not touched. (The 2x is worth stating rather than
 * folding into the number, because it means the price a CONSUMING flow actually pays is two points per second
 * where FLOW_AGE_RATE says one. That is a pricing question, not a resolution one, and it is untouched here.)
 * The only term left with sub-second resolution was the optimism bonus, whose sole mover is
 * COMPLETING a unit of work. A flow reaches its queued jobs only with `frame == NULL` (engine.c's job arms are
 * all under it) and is credited a completed unit under the SAME predicate, so the one flow that can run a job
 * is the only kind of flow that pays anything at all between two whole seconds, and it pays once per job. Three
 * runs of one login page, one fresh browser each: `jobsQueued` 373, 401, 417 — a network quantity — and
 * `jobsRun` 14, 14, 14. The job holder climbs 1/(1+v) until it passes the frontier's FROZEN maximum inherited
 * visit count (a mid-program arm completes no unit, so its count never moves and the wall never lowers), and
 * stops there: an integral, document-determined number with no network in it. The deficit that locks it out is
 * 1/15 − 1/14 = 0.0048 points — less than one quantum of service — and a unit of one second cannot repay it
 * with anything short of a full point, so it was never repaid at all.
 *
 * THE PROSE FOR THIS WAS ALREADY IN THE TREE, WHICH IS THE OTHER HALF OF WHY THIS IS A RESTORATION AND NOT A
 * NEW POLICY. FLOW_SERVICE_US's own preamble above still says "rank moves one notch per quantum of service, so
 * the flow that holds the lead keeps it for a quantum and a tied rival then takes its turn"; flow.h writes the
 * weight out as `1/(1+v) - (s+F)*Q*RATE` and states that "the first quantum costs a candidate 0.012"; and
 * flow.h's census paragraph reasons that raising every member's service by delta drops every weight by
 * `delta * FLOW_SERVICE_US * FLOW_AGE_RATE`. All three describe a notch of one QUANTUM. The implementation
 * moved to one notch per second when the aging acquired its family half and none of them moved with it — a
 * mechanism replaced with the prose for the old one left standing, which is exactly the scar this file's own
 * rules say to delete rather than to read as a gap.
 *
 * THE STEP MAY NOT COST MORE THAN AN EMISSION, which is what the compile-time relation below now says and is
 * the one thing the two constants genuinely owe each other: at a step of one quantum the aging moves
 * FLOW_AGE_QUANTUM points per quantum, and if that exceeded 1.0 a single slice of the thread would outweigh a
 * finding and the reward term could never order anything. Asserted at COMPILE time because it is a relation
 * between two constants and no run can tell you it broke. */
#define FLOW_SILENCE_US ((int64_t)1000000)
typedef char flow_quantum_costs_at_most_one_finding[(FLOW_SILENCE_US >= FLOW_SERVICE_US) ? 1 : -1];
#define FLOW_AGE_RATE (1.0 / (double)FLOW_SILENCE_US)
/* …AND THE PRICE OF ONE NOTCH, WHICH IS THE PRICE OF ONE QUANTUM. The aging term is `notch * this`, so the
 * exchange rate is untouched — a flow still ages by exactly `us * FLOW_AGE_RATE` in the long run, so which
 * flow wins over any real interval is the decision it always was — and what changes is only that the order can
 * now express the smallest amount of thread time it ever hands out. FLOOR-then-multiply, never
 * multiply-then-floor: two flows on the same notch get the identical product, which is what makes the
 * fork's rank-neutrality equality below an exact one. */
#define FLOW_AGE_QUANTUM ((double)FLOW_SERVICE_US * FLOW_AGE_RATE)

/* HOW MANY WHOLE QUANTA OF THE THREAD THIS FLOW HAS ACTUALLY CONSUMED — the ONE quantised reading of `cpu`, and
 * a CENSUS quantity: it says who is consuming the thread, at the granularity the thread is handed out in. It is
 * no longer a term of the weight, which is the correction this comment carries: it used to be the OPTIMISM
 * term's "visits", and a microsecond is not a visit (see flow_weight). */
int64_t flow_service_notch(const Flow *f) { return f->cpu / FLOW_SERVICE_US; }

/* …AND THE SAME READING OF THE FORK FAMILY'S: how many whole quanta this flow's family has burned since any of
 * its arms last emitted. Two functions because they are two quantities with two reset points (flow_age_running
 * says why), and both are the census's — read together, their RATIO is the fork factor of the widest family in
 * the frontier, which is a fact about the document's branching. The AGING term reads their SUM, in the SAME
 * unit, below: the census and the order are no longer denominated in different quantities, which is what let a
 * census row and a rank disagree about how much service a member had been charged for. */
int64_t flow_family_notch(const Flow *f) { return acct_family_us(f) / FLOW_SERVICE_US; }

/* HOW MANY WHOLE COOPERATIVE QUANTA OF SILENCE THIS FLOW STANDS AT — the AGING term's quantity, and the only
   reading of it the weight makes. It is the SUM of the two notches above and it is in THEIR unit, which is the
   granularity at which the thread is handed out: a term that stepped coarser than that would be a charge the
   pick following it cannot see (FLOW_SILENCE_US says what that cost, in jobs never run). The PRICE is applied
   where the term is summed, at FLOW_AGE_QUANTUM, so this stays an exact integer and nothing quantises a rank
   on the FPU.
   THE SUM IS THE SILENCE. `cpu` is what THIS FLOW has burned since its own last emission and `fam_us` is what
   its whole fork family has burned since ANY of its arms last emitted, and the aging is both because each
   answers a comparison the other cannot: the family half orders one fork chain against another (without it a
   family that emitted V and wears N names pays for V once and presents it N times), and the own half orders a
   family's members against each other (without it every arm reads the identical number, so on a frontier that
   IS one family — which a real page's is, every flow descending from the boot flow — the term cancels out of
   every comparison it appears in and §scheduler's monopolizer sentence has nothing left to compare).
   `int64_t` throughout: at 12 ms a quantum this counts quanta of silence, so it cannot overflow anything a
   session can reach, and it is exact — the price is applied to it, never inside it. */
int64_t flow_silence_notch(const Flow *f) {
    return (f->cpu + acct_family_us(f)) / FLOW_SERVICE_US;
}

/* THE FLOW'S PLACE IN THE QUEUE — every term of the weight that is a TAG rather than a reading, which is
   exactly the set an ARRIVAL has to reproduce. It is split out of flow_weight for one reason and it is the
   defect that forced it: flow_arrive_at_virtual_time's own prose says it "assigns every term the weight is made
   of", and it assigned three of four, leaving the REWARD at zero. Its assert was a one-sided `<=`, so the
   omission could only ever push a newcomer DOWN and the assert passed however far down that was — while
   flow_fork_inherit's parallel assert is an EQUALITY over flow_weight and would have fired at the first fork.
   MEASURED, and the number is what a one-sided assert cannot see: on the smoke fixture every fork carried the
   boot family's reward, so a 401-member frontier stood at `valMax` 118 and the ONLY members at zero were the 12
   @S candidate sessions — `valZero:12` beside `cands:12`, the two counts identical because they are the same
   twelve flows. Their weight was 0.976 for the whole run against a `wTop` that never fell below 56, and the
   report said so in the field that exists to say so: `turns:0` on every one of them, after 254,181 switches.
   §@S's whole search — the delivery probe, the derived breakout, the filter-survival distance, the fire — runs
   only inside a candidate flow, so on any document whose exploration keeps emitting, every one of those
   mechanisms was unreachable by construction and nothing crashed.
   WHY THE FITNESS IS NOT IN HERE, and it is not an exception carved for convenience. §scheduler calls the
   distance "a fraction, READ at the pick and never accumulated"; a tag is a POSITION a newcomer can be placed
   at, and a per-payload reading is not one — the payload a newcomer carries is not the payload the flow in
   service carries, so there is nothing for it to arrive at. It is the one term the arrival names rather than
   copies, and naming it is what keeps the assert below an exact equality instead of a tolerance.
   EXACT IN INTEGERS AND IN FLOATS. `val` and `visits` are copied verbatim by both entries, and the silence
   notch is an integer division of copied integers, so two flows that arrived at one another compare EQUAL here
   with no epsilon — which is the same property flow_fork_inherit's assert already rests on. */
static double flow_queue_weight(const Flow *f) {
    double reward = f->val;
    /* §scheduler'S THREE QUANTITIES, AND THIS FUNCTION USED TO HOLD TWO. "accumulated emitted VALUE + a UCB
       optimism bonus ∝ 1/(visits+1) − CPU-AGING" names emitted value, VISITS and CPU; the optimism term read
       `cpu / FLOW_SERVICE_US`, so thread time was in both non-reward terms at two scopes and the visit count
       did not exist. The consequence is not a rounding difference, it is which term GOVERNS. FLOW_AGE_RATE
       prices one second of unproductive CPU at one finding, so the aging term gives a silent flow about a
       second before an unrun sibling passes it; the optimism term keyed on service did it after ONE quantum,
       12 ms. The priced budget was dominated eighty-three to one and decided nothing, and what a flow actually
       got was 12 ms to reach its first emission.
       WHAT THAT COST, MEASURED ON REAL PAGES rather than a fixture. A flow reaches its queued jobs only with
       `frame == NULL` (engine.c: every job arm is under that test), so a promise reaction runs only after its
       holder finishes the program it is inside, and a modern bundle's whole fetch surface hangs off reactions.
       Under the service-keyed bonus a flow was strictly outranked by every arm it forked as soon as it crossed
       one quantum, and those arms were outranked by the arms they forked inside their own first quantum, so the
       set of members at notch 0 was refilled by branching faster than a quantum drained it and no flow ever
       received a SECOND quantum. One real login page: 4650 flows, 3093 switches — under one turn per flow —
       217 jobs QUEUED and ZERO run in ninety seconds, no endpoints. A page whose frontier stayed at 13 flows
       ran all 32 of its jobs and learned 4 endpoints, and a third at 17947 flows learned none: the correlation
       is with jobs RUN and the anti-correlation is with frontier size, which is exactly this arithmetic. */
    /* OPTIMISM DECAYS WITH COMPLETED UNITS OF WORK — §scheduler's own word is "visits", and a visit is a TRIAL.
       A flow that was handed the thread and preempted in the middle of a program has not completed one: it is
       the same trial, still running, and charging it as a visit is what made the term a second clock. What a
       flow has actually finished is the quantity this reads (flow.h's `visits`, credited by the scheduler at
       the one boundary that can see it), and the two guarantees §scheduler names then belong to two terms
       instead of one.
       IT IS NOT THE DISPATCH COUNT THIS FILE ONCE MEASURED, and the distinction is the whole of why that
       measurement does not refute this. Keyed on dispatches the bonus fell 0.167 the first time the scheduler
       CHOSE a flow, so the act of switching was enough to make it no longer the best one; a dispatch is a
       decision by the scheduler and says nothing about the flow. A completed unit is a fact about the flow, and
       it is the fact the reward is a function of: a program that ends is a program whose fetches were reached,
       whose microtask checkpoint is due and whose queued reactions can now run.
       THE TIE IS WHAT MAKES THIS FAIR RATHER THAN GREEDY. An arm inherits its parent's count, so at the branch
       the two are equal and flow_pick's STRICT comparison leaves the thread where it is; the parent finishes
       the unit it is inside, its count advances, and every arm — and every never-run member — then STRICTLY
       outranks it and is picked. So a flow monopolises exactly one unit of work and no more, which is
       §scheduler's "a deep loop suspends and yields so siblings run, then resumes" measured in the unit the
       page's own code is written in. What it may NOT do is monopolise a unit that never ends, and that is the
       aging term's job below — not this one's. */
    int64_t visits = f->visits;
    double ucb     = 1.0 / (1.0 + (double)visits);
    /* THE AGING IS THE FLOW'S OWN SILENCE **AND** ITS FAMILY'S, and the second half is what the family charge
       could never do alone. The family reading was landed for a real defect and stays: `val` is copied to every
       arm (flow_fork_inherit), so a family that emitted V with N live arms presented V exactly N times, and the
       aging meant to cancel it was charged to whichever arm held the thread — measured at 686 members, `svcMax`
       1124 notches against `svcFamMax` 8910, a factor of 7.93, with `candSvcMax` 1 (the whole @S search got
       about one quantum in fifteen minutes). Charged to the family, a fork chain is one accounting unit against
       every OTHER family, which is what that reading required.
       AND IT IS BLIND INSIDE ONE, WHICH IS WHERE THE MONOPOLIZER ACTUALLY IS. Every arm of a family reads the
       identical `fam_us`, so the term is a constant added to every member's weight and CANCELS out of every
       within-family comparison. On a real page the whole frontier is one family — every flow descends from the
       boot flow — so §scheduler's "a monopolizer that burns CPU without emitting sinks below productive+unrun
       flows" was, exactly where it is needed, a sentence with nothing left to compare. The flow's OWN service
       since its last emission is what restores the comparison, and it is the same currency and the same reset
       point (flow_credit_emit zeroes both), so the two halves are one quantity read at two scopes rather than
       two policies.
       IT IS ALSO WHAT KEEPS THE OPTIMISM TERM HONEST NOW THAT IT COUNTS UNITS. A flow inside a program that
       never ends completes no unit, so its bonus never decays; without an own-service charge it would hold the
       thread for ever and the visit count would be the reset-by-not-finishing that this file forbids the fork
       tree. With it, such a flow sinks at FLOW_AGE_RATE's price for every microsecond of unproductive CPU and
       is passed by its siblings — the case that price was written for, finally governed.
       IT STEPS IN COOPERATIVE QUANTA AND IT USED TO STEP IN WHOLE REWARD POINTS, WHICH IS THE HALF THAT
       DECIDES WHETHER ANY OF THIS WORKS. Quantising at all is required — a weight that moves continuously
       falls below a tied rival between two consultations of the preempt hook, which is thrash at a finer grain
       — and the argument that used to stand here went on to reject the QUANTUM as the unit, on the ground that
       "a term that steps every 12 ms therefore hands the thread on every 12 ms". That is not a defect; it is
       start-time fair queueing, and it is §scheduler's own sentence — "a deep loop suspends and yields so
       siblings run, then resumes". What made it look like a defect was the tree it was written against, in
       which a newborn arm entered at the FRONT of the queue; flow_fork_inherit closed that at the fork, and
       with an arm entering at its parent's virtual time a handover per quantum is the queue rotating, not
       thrashing. It costs no extra switch either: the cooperative quantum ALREADY returns the thread at
       exactly this granularity, so the aging-driven yield now coincides with the slice expiry instead of
       firing once per 42 of them.
       WHAT THE POINT-WIDE STEP COST, and it is the reason this is a correction rather than a preference. A
       step of one whole point is 83.3 quanta of silence, which a CONSUMING flow accrues in 41.7 quanta of its
       own CPU (the charge bills `cpu` and `fam_us` both, and the notch is their sum), so a flow that consumes
       a quantum has its rank UNCHANGED and the pick that immediately follows re-reads the weight the charge
       did not move — 40 times out of 41. The order then rests
       entirely on whatever else has sub-second resolution, and the only such term is the optimism bonus —
       whose sole mover is COMPLETING a unit of work, which is also the sole precondition for reaching a queued
       job (`frame == NULL`, engine.c). So the one flow that could run a promise reaction was the one flow
       paying anything, its rivals were mid-program arms frozen at the visit count they inherited, and it was
       locked out by a deficit of 1/15 − 1/14 = 0.0048 points that a point-wide step could not repay with less
       than a full finding. Measured: `jobsRun` 14, 14, 14 across three runs of one login page whose
       `jobsQueued` was 373, 401, 417.
       THE PRICE IS UNTOUCHED AND IT IS APPLIED HERE. One second of unproductive thread time still costs ONE
       EMITTED FINDING (FLOW_AGE_RATE), and an emission is still worth exactly 1.0 (flow_credit_emit asserts
       it); what moved is the STEP, from a point to a quantum, which is 0.012 of a point. The two are separate
       constants now precisely because one number could not be both without one of them being wrong.
       WHAT THAT BUYS, TERM BY TERM. A flow that finishes its unit of work inside its quantum keeps the thread
       to the end of it and hands over on its VISIT — which is the optimism term doing the fairness. A flow
       inside a unit that never ends completes no visit, so the optimism term can never demote it, and this is
       what does: it sinks 0.012 per quantum it consumes and its arms, which inherited a smaller silence, pass
       it at the first quantum rather than at the 84th. Neither term can do the other's job.
       O(1) to read: every flow points straight at its family's root, so this adds one indirection to the pick
       and no walk. It is why the preempt hook's seam assertion snapshots this notch and the visit count
       (engine.c) — between two of them a flow's weight cannot move except through an emission. */
    return reward + ucb - (double)flow_silence_notch(f) * FLOW_AGE_QUANTUM;
}

/* THE ORDER ITSELF — the queue coordinate above plus the one term that is a READING of the item rather than a
   TAG on the queue. There is still exactly one comparator: every caller ranks by this, and the split names
   which half an ARRIVAL can be placed at.
   §@S'S FITNESS, READ WHERE A FITNESS IS READ. "The search is DISTANCE-DIRECTED (a fitness of
   {filter-survived, sink-reached, context-escaped, handler-fires} the WFQ reads)" — and this function read
   `val`, so what the WFQ actually read was a LEDGER of what each search had learned. The two are different
   quantities and the difference is not academic: a ledger must be paid at most once per observation or it
   reorders the frontier on repetition, and that same rule makes every candidate that merely EQUALS an
   earlier one worth exactly what an unstarted flow is worth. So a search's second, third and tenth
   breakouts — the ones a derivation produced precisely because the first did not fire — ran with the
   ordering blind to how far each of them got, and §@S's "a near-miss is mutated toward the gap; a dead
   candidate starves" had no comparison anywhere to be true of. This term is that comparison, and the
   ledger above is untouched by it: nothing is paid here, nothing accumulates, and two flows standing at
   the same distance rank equal, which is the correct answer and the one a once-only credit cannot give.
   IT IS BOUNDED BY THE OPTIMISM TERM'S OWN RANGE for the reason that term is bounded by one emission: a
   fitness says how promising an item is, not how much it has produced, so it may never outweigh a finding.
   At its maximum a candidate whose bytes arrive intact ranks with a never-run flow — enough to be picked
   out of a saturated frontier, not enough to hold the thread against a flow that has emitted. It is
   charged the same aging as everything else, so it buys a candidate turns, never a monopoly.
   ZERO FOR EVERY FLOW THAT IS NOT A CANDIDATE, which is not a special case in the arithmetic: a flow with
   no payload has no distance to be a fraction of, and flow_set_distance asserts that rather than allowing
   one to be recorded. */
double flow_weight(const Flow *f) {
    return flow_queue_weight(f) + f->cand_dist;
}

/* THE SILENCE AT WHICH A FLOW IS GUARANTEED TO RANK BELOW ANY NEVER-RUN SIBLING — the crossing §scheduler's
   sentence claims exists, computed from the flow's OWN reward rather than a fixed threshold that could fire
   falsely on a very productive flow. A never-run flow carries the full optimism bonus and no aging, so its
   weight is its reward plus its fitness distance plus 1.0 and therefore at least 1.0; past this charge a
   serviced flow's aging has eaten its whole reward and the comparison cannot go the other way for any reward,
   which is why the crossing closes strictly.
   AND IT IS COMPARED AGAINST BOTH HALVES OF THE SILENCE — this flow's own and its FAMILY's — which is what
   makes the assertion able to catch a fork chain AND one of its arms. A walker whose arms burn the document and
   depart, or simply one with N live arms, shows a per-arm `cpu` that is a sliver of what the family has
   consumed (svcMax 1124 against svcFamMax 8910 on the frontier the family charge was added for); read against
   that sliver alone this was an identity that held for a family monopolising the thread for hours. Read against
   the family alone it is an identity that holds for every member of a one-family frontier at once, which is the
   ordinary state of a real page. The sum is the only reading that is neither.

   THE BOUND IS DERIVED FROM flow_weight, NEVER FROM THE SENTENCE ABOUT IT, AND THE DERIVATION IS REDONE
   WHENEVER THE FORMULA MOVES — which is the only reason this stays a check rather than becoming a number
   somebody once believed. If any candidate is unrun — ALL of its terms at zero, its visit count, its own
   service and its family's, which is what flow_pick's `unrun` tests — its weight is its own reward plus its
   own distance plus 1.0, both of which are non-negative, and hence at least 1.0; so the MAXIMUM the pick
   returns is at least 1.0 too. Write that out for the picked flow,
   with `v` its visit count, `D` its fitness distance, `A` its silence notch IN QUANTA and `Q` the price of one
   quantum (FLOW_AGE_QUANTUM): `val + D + 1/(1+v) − A*Q >= 1`, and since `1/(1+v) <= 1` it forces
   `A*Q <= val + D`. `D` is a FRACTION (flow_set_distance asserts [0,1]), so `A*Q <= val + 1` and hence
   `A <= (val + 1) / Q`. `A` is a FLOOR of `(cpu + fam_us) / S` with `S` the cooperative quantum, so
   `cpu + fam_us < (A + 1) * S = (val + 1) * FLOW_SILENCE_US + S`. That is this expression.
   THE TWO TERMS ARE EARNED SEPARATELY AND THE SECOND ONE JUST SHRANK BY A FACTOR OF 83, which is the whole
   visible consequence of the aging's step becoming the quantum. The `(val + 1)` second is the ENTIRE RANGE of
   the fitness comparator on top of the reward: a candidate whose bytes arrive intact ranks a full point above
   one whose bytes died, so it may outlast one by a point's worth of silence, and that is the term doing
   exactly its job. The `+ S` is the width of the aging's OWN step, because a bound cannot be tighter than the
   resolution of the quantity it bounds — and that step used to be a whole SECOND, so this bound used to allow
   a second of unmeasured silence that nothing in the formula could account for. Neither term is slack, and if
   the comparator's range ever stops being 1 this arithmetic is what has to be redone.
   THE QUANTITY IT MEASURES IS THE SILENCE, WHICH IS BOTH HALVES OF THE AGING AND USED TO BE ONE. It read the
   family's service alone, which is the half that CANNOT order a family's own members, so on a frontier that is
   one family this was an identity with the interesting case removed. The name says which quantity it bounds,
   because a bound named for one of two summands is the shape that goes quietly wrong when the other is added.
   WHAT IT CATCHES IS AN EDIT, NOT A STATE. The derivation above is an identity under the current formula, so it
   can only fire on a change that breaks it — a clamped or capped aging term, an optimism term without a ceiling
   of 1, a reward allowed to grow with the CPU it is weighed against, a fitness comparator that is not a
   fraction, or an optimism term keyed on something a never-run flow does not stand at zero of. Each of those
   is exactly the shape a "fix" for a thrashing
   scheduler takes, which is why the guard is worth its arithmetic; it is not a thing that fires on today's
   tree, and an earlier comment saying it FIRES on the tree it replaced was the stale-DFAIL shape wearing an
   assertion.
   `static inline` so release (where the DCHECK's condition is unevaluated) does not warn. */
static inline int64_t flow_silence_us_to_sink(const Flow *f) {
    return (int64_t)((f->val + 1.0) * (double)FLOW_SILENCE_US) + FLOW_SERVICE_US;
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
    /* AND THE RANKING CHANGED, WHICH IS THE THING A MARK IS AND NOBODY SAID SO. §scheduler's value yield fires
       "the moment a parked flow outranks", and the WFQ's answer is `the best ELIGIBLE flow` — so the eligible
       SET is as much a term of that answer as any weight is. A mark takes a member out of the pick and a clear
       puts one back, and neither raised the one signal every consumer of the ranking keys on. Both consumers
       were wrong in opposite directions and only one of them could say so: the preempt hook caches its rival on
       this generation, so it kept ranking against a set that no longer existed, and the value-yield assertion
       snapshots this generation, so it had no way to distinguish "the set changed" from "nothing changed" and
       aborted the smoke on a yield that was entirely correct. See the assertion in engine.c's preempt_hook.
       IT GOES IN THE EXISTING GENERATION RATHER THAN A FIFTH CLAUSE BESIDE IT. §scheduler is ONE WFQ policy,
       and `frontier_rank_changed` is already the one sentence that means "the running flow's claim on the
       thread may have changed" — a second sentinel tracked in parallel would be a second definition of the
       same event, free to drift exactly where these two already had. */
    frontier_rank_changed();
}

/* THE HOST ANSWERED THIS FLOW — see flow.h for why the clear is per flow and per EVENT. */
void flow_clear_host_owed(Flow *f) {
    DCHECK(f != NULL, "a host event cleared the mark of no flow at all");
    f->owed_gen = 0;   /* never equal to g_owed_gen, which starts at 1 and only ever moves forward */
    /* …AND THIS IS THE DIRECTION THAT ACTUALLY BROKE, which is why it is not enough to say "a mark change is a
       ranking change" at the site above and leave this one implicit. A clear makes a flow ELIGIBLE that the
       scheduler's last pick could not consider, and the pick and the preempt hook do not run at the same
       instant: the loop picks the best of the unmarked members, and a clear that lands during that flow's very
       first step — flow_deliver_one_reply settles the shared document-script slot for every flow waiting on one
       address — hands the hook a rival the pick was never shown. The hook then yields against a flow the
       scheduler chose one step earlier, with the frontier generation, the service notch, the family notch and
       the reward all exactly as they were. That is the pick and the hook answering one state two different
       ways, it is what the value-yield assertion exists to catch, and it is what it caught. */
    frontier_rank_changed();
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
    /* THE WHOLE-FRONTIER FORM OF THE SAME STATEMENT — every member of the frontier just became eligible, which
       is the largest ranking change this engine can make in one call, and it raised nothing. The host reaches
       this between slices (engine_set_referenced, and the provider's own edges), so the yield request it raises
       is read at the first opcode of the next slice, which is exactly when the new set first matters. */
    frontier_rank_changed();
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
    /* A MEMBER CARRYING THE FULL BONUS AND NO AGING — its weight is its reward + 1.0 and hence at least 1.0,
       which is the only property the assertions below need, and the test is written as exactly that premise:
       EVERY non-reward term of flow_weight at zero, read through the same functions the weight reads.
       IT USED TO BE `cpu == 0` ALONE, which was the whole test while `cpu` was the whole of both non-reward
       terms. The family charge made it two coordinates and the visit count made it three, and each time,
       testing fewer than all of them left the assertions resting on a premise the frontier stops satisfying —
       a flow at `cpu == 0` inside a family a SIBLING has since burned is worth `val + 1.0 − aging`, which can
       be deeply negative — so the guard would have started firing on healthy runs. Reading the notch rather
       than its summands is what makes this survive the next such change: the population named here is the one
       the derivation is about, however many quantities the notch is built from. */
    int unrun = 0;
    DCHECK(!(seed && worst), "the eviction tail was asked with an incumbent to defend — the seed states who "
                             "keeps the THREAD, and the flow the pager gives up is not a question about that");
    /* THE SEED IS ELIGIBLE ON THE SAME TERMS AS EVERY CANDIDATE — a member of the frontier, and not one that
       has told the scheduler it can make no progress. A flow that answered OWED is the one case where the
       incumbent must NOT keep the thread: keeping it is the spin the mark exists to end. */
    if (seed && flow_is_member(seed) && !(runnable_only && flow_host_owed(seed))) {
        best = (Flow *)seed; bw = flow_weight(seed);
        if (seed->visits == 0 && flow_silence_notch(seed) == 0) unrun = 1;
    }
    for (int i = 0; i < g_flows_n; i++) {
        double w;
        if (g_flows[i] == exclude || g_flows[i] == seed) continue;
        /* NOT A DROP AND NOT A DEPRIORITISATION: the flow keeps its weight, its place and every work item it
           holds, and it is picked again the moment anything could have answered it (flow_clear_host_owed). */
        if (runnable_only && flow_host_owed(g_flows[i])) continue;
        w = flow_weight(g_flows[i]);
        if (g_flows[i]->visits == 0 && flow_silence_notch(g_flows[i]) == 0) unrun = 1;
        /* THE TAIL IS THIS SCAN READ THE OTHER WAY, which is the whole of why the pager has no ranking of its
           own. The flow the cold tier gives up first must be the flow the WFQ would have run last, or the
           engine would page out a member the scheduler still wanted and keep one it was starving; a size
           estimate, an age or a "cheapest to rebuild" score would be exactly that disagreement. One comparator,
           one scan, one direction bit. */
        if (!best || (worst ? w < bw : w > bw)) { best = g_flows[i]; bw = w; }
    }
    /* §scheduler'S SENTENCE, ASSERTED WHERE THE CHOICE IS MADE — "CPU-AGING so a monopolizer that burns CPU
       without emitting sinks below productive+unrun flows". This is the one line in the engine that decides
       which flow runs, so it is where the claim is either true or a comment. WHAT IT CATCHES IS AN EDIT TO
       flow_weight, not a state of today's frontier: the bound is DERIVED from that function (see
       flow_silence_us_to_sink), so under the formula as it stands it is an identity, and it fires the moment an edit
       breaks the derivation — a clamped or capped aging term, an optimism term whose ceiling is no longer 1, a
       reward allowed to grow with the CPU it is weighed against. Each of those is exactly the shape a "fix" for
       the thrash this quantum handles would take, and each would let the walk this rate was priced against be
       re-picked for 227 seconds with unrun siblings waiting, which is what it did on the tree this replaced. */
    DCHECK(worst || !best || !unrun ||
           best->cpu + acct_family_us(best) < flow_silence_us_to_sink(best),   /* see the derivation there */
           "the WFQ re-picked a flow that has burned more thread time since its last emit — its own plus its "
           "fork chain's — than its entire accumulated reward is worth, while a never-run flow was waiting — "
           "the aging term is no longer commensurate with the reward it is subtracted from, so a monopolizer "
           "cannot sink");
    /* …AND THE OTHER HALF OF §scheduler'S PAIR, WHICH NOTHING WAS ASSERTING: "a UCB optimism bonus ∝
       1/(visits+1) so a NEVER-RUN FLOW IS NEVER STARVED". The bound above says a silent monopolizer sinks; it
       says nothing about the term that is supposed to lift the flow waiting behind it, and the two are separate
       claims that can break separately. It is the same KIND of guard as the one above and it is written the
       same way — an identity derived from flow_weight, so what it catches is an EDIT rather than a state: an
       untouched member's weight is `val + 1.0` with both aging halves and the visit count at zero, hence at
       least 1.0, and `best` is the maximum over the very scan that set `unrun`, so the floor holds by
       arithmetic. It is FALSE the moment the optimism bonus is keyed on a quantity a never-run flow does not
       stand at ZERO of — which is precisely what a bonus keyed on thread time was, since a from-baseline flow
       arrives at the running flow's service (flow_arrive_at_virtual_time) rather than at nothing — or given a
       range other than 1.0, or made to depend on anything the aging term already prices. Each of those is a
       way for "never starved" to stop meaning anything while every other assertion in this file still passes,
       and one of them is how a frontier of 4650 members came to have under one turn each. */
    DCHECK(worst || !best || !unrun || flow_weight(best) >= 1.0,
           "the WFQ picked a flow worth less than an untouched member of its own frontier — a never-run flow "
           "carries the whole optimism bonus and no aging, so nothing may be picked ahead of it below that "
           "floor; the optimism term is no longer the one §scheduler prices a never-starved flow against");
    return best;
}

/* WHAT THE ORDERING IS MADE OF — see flow.h for what each row answers and why `val` alone cannot answer it.
 *
 * IT IS A SEPARATE SCAN AND NOT A FIFTH QUESTION FOR flow_pick, which is the opposite of the rule one function
 * up and is deliberate: flow_pick's four questions are all the ORDER, so they must share one comparator or
 * they can disagree about which flow runs. This one decides nothing and reads the order's INPUTS, so sharing
 * that scan would mean giving the pick accumulators it does not need and a shape a census could drift into
 * steering. It reports the terms AND their sum — flow.h says why the sum had to be added, and the sentence that
 * stood here ("it calls flow_weight for nothing at all") had already stopped being true of the loop below.
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
    out->svc_max = out->svc_min = out->svc_fam_max = 0;
    out->vis_min = out->vis_max = 0;
    out->cand_members = out->cand_unrun = out->cand_dec_max = 0;
    out->cand_svc_max = 0;
    out->dec_max = 0;
    out->w_top = out->w_min = out->cand_w_max = 0.0;
    for (i = 0; i < g_flows_n; i++) {
        const Flow *f = g_flows[i];
        int64_t s = flow_service_notch(f);
        /* THE SUM THE PICK USES, not only the terms it is made of — see flow.h for the reading that went wrong
           without it. One call per member, and this scan still decides nothing. */
        double w = flow_weight(f);
        /* HOW MANY DECISIONS THIS FLOW STANDS ON, read from wherever its decision state currently lives: a
           parked flow's blob, and decide.c's live globals for the one the scheduler is switched into — the
           same split cold.c's census makes, because there is only one place each can be. Asked of EVERY
           member and not only the candidates, because the candidates' figure is meaningless without it. */
        long dec = 0;

        if (f->dec_blob) decide_blob_stats(f->dec_blob, &dec, NULL);
        else if (f == g_running) decide_live_stats(&dec, NULL);
        if (dec > out->dec_max) out->dec_max = dec;

        DCHECK(f->val_born <= f->val,
               "a member of the frontier holds more inherited reward than reward — its birth mark and its "
               "account disagree, so `self_emit` below counts something that was never an emission");
        if (i == 0 || f->val < out->val_min) out->val_min = f->val;
        if (i == 0 || f->val > out->val_max) out->val_max = f->val;
        if (f->val == 0.0) out->val_zero++;
        if (f->val > f->val_born) out->self_emit++;
        if (f->cpu == 0) out->unrun++;
        /* THE OPTIMISM TERM'S OWN COORDINATE — completed units of work, which no service row can stand in for
           (flow.h). `vis_max == 0` on a frontier of thousands is the statement that not one member has reached
           the end of a program, and therefore that not one queued job can have run, whatever the switch and
           fork counts say. It is the number that separates "being served fairly" from "finishing nothing". */
        if (i == 0 || f->visits < out->vis_min) out->vis_min = f->visits;
        if (f->visits > out->vis_max) out->vis_max = f->visits;
        if (s > out->svc_max) out->svc_max = s;
        /* …AND THE FAMILY THIS MEMBER BELONGS TO, in the SAME notch — which is now the notch the AGING term
           actually reads, so `svc_fam_max` against `svc_max` is no longer a diagnostic beside the order, it is
           the order's own denominator against one member's share of it. Read per member rather than per family
           because a family has no member of its own to be asked, and the maximum over members reaches every
           family exactly as often as it has arms standing. */
        {
            int64_t fam = flow_family_notch(f);
            if (fam > out->svc_fam_max) out->svc_fam_max = fam;
        }
        /* THE FLOOR, TAKEN OVER EVERY MEMBER AND NOT ONLY THE SERVED ONES — see flow.h. A frontier holding one
           never-run flow reads 0 here and that is the answer, not a hole in it: the aging term is then charging
           the rest of the frontier against a member that has burned nothing, which is exactly the case the term
           was priced for. It is `svc_max` read the other way, so it shares its scan and its unit. */
        if (i == 0 || s < out->svc_min) out->svc_min = s;
        if (i == 0 || w < out->w_min) out->w_min = w;
        /* AND THE SAME QUESTIONS ASKED OF THE CANDIDATES ALONE — see flow.h. `cand_src` is what a candidate
           session IS (the substitution it carries), and engine.c copies it to a sibling, so this counts the
           search's whole live population rather than its roots. */
        if (f->cand_src) {
            if (!out->cand_members || w > out->cand_w_max) out->cand_w_max = w;
            out->cand_members++;
            if (f->cpu == 0) out->cand_unrun++;
            if (s > out->cand_svc_max) out->cand_svc_max = s;
            if (dec > out->cand_dec_max) out->cand_dec_max = dec;
        }
    }
    top = flow_best();
    if (top) { out->val_top = top->val; out->w_top = flow_weight(top); }
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
    DCHECK(f->frame == NULL && f->parked == NULL,
           "a flow was removed holding a live frame or parked continuations — its whole activation chain, and "
           "everything those frames close over, would be retained by a handle nothing will ever free");
    DCHECK(JS_IsUndefined(f->jobs) && JS_IsUndefined(f->pending) && JS_IsUndefined(f->perform_q) &&
           JS_IsUndefined(f->deliver_q),
           "a flow was removed still holding queued jobs, pending host replies, unstarted cross-agent "
           "operations or routed cross-document messages — each is a work item on the one frontier, and the "
           "WFQ may never drop one");
    /* …AND THE ONE FIELD BESIDE THEM THAT IS NOT WORK, asserted separately BECAUSE it is not work: merging it
       into the line above would put a message on it that is false about this field ("the WFQ may never drop
       one" — a commitment is nobody's work item). What is true of it is the structural half both lines share:
       a JSValue field added to Flow gets a line in flow_release and a line here, so a caller that skipped the
       release crashes at the removal instead of holding an Array and its every string for the process. */
    DCHECK(JS_IsUndefined(f->deliver_world_q),
           "a flow was removed still holding the record of which sending timelines it was in — flow_release "
           "frees that Array and every [vector, taken] pair it names, so a flow reaching here with one was "
           "removed without being released");
    DCHECK(f->dyn == NULL && f->dyn_cand == NULL && f->dyn_type == NULL && f->dyn_url == NULL &&
           f->dyn_el == NULL && f->dyn_doc == NULL && f->dyn_token == NULL &&
           f->dyn_pos == NULL && f->dec_blob == NULL && f->pin_blob == NULL,
           "a flow was removed with its lazily-loaded chunk bodies or its suspended decision/pin blobs still "
           "attached — the flow's own allocations, freed by nothing else");
    for (int i = 0; i < g_flows_n; i++) {
        if (g_flows[i] == f) {
            JS_FreeValue(ctx, f->fn);
            /* THE ROUTED RECORD AND ITS ORIGIN STAMP USED TO BE FREED HERE, and that free is gone rather than
               converted: it ran on a work item the frontier was dropping and made the drop invisible, which is
               the one thing this file's asserts exist to stop. The queue is released in flow_release beside
               the operation queue, and its emptiness is asserted just above. */
            /* THE CANDIDATE IT WAS VERIFYING. Both strings are this flow's own copies, and the only other place
               that frees them is the frontier's teardown — which walks the flows that are STILL THERE, so a
               flow removed here took them with it into nothing. `cand_sink` is static text and is not one. */
            free(f->cand_src); free(f->cand_payload);
            /* AND ITS PLACE IN THE FORK TREE. THE THREAD TIME IT BURNED IS ALREADY WHERE IT BELONGS — every
               microsecond went onto the family's account as it was consumed (flow_age_running), so a departing
               arm has nothing left to hand anybody and the hand-up that used to stand here would bill the
               family twice for work it has already been charged for. What is left is the node's LIFETIME: the
               owner mark comes down so a compression can walk past it, and the reference goes. This is the ONE
               exit a Flow has, which is what makes it the one place that can be said. */
            acct_depart(f);
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

/* See flow.h. THE WHOLE COLUMN AND NOT THE TAIL: `script_i` is where the flow HAS GOT TO, and a row behind the
   cursor is one it has already compiled — but a row it has compiled is a program of that document whose frames
   it may still be suspended inside, which is a fact about the same realm. The question this answers is "does
   any member of the frontier still name this document at all", so it counts every row, and a caller that wants
   "still to run" would be asking a different question with a different assert behind it. */
int flow_programs_for_document(uint32_t doc) {
    int n = 0;

    for (int i = 0; i < g_flows_n; i++) {
        const Flow *f = g_flows[i];
        /* A FLOW WITH NO QUEUE HAS NO COLUMN — the parallel arrays are allocated together at the first row, so
           `dyn_n` is what says whether `dyn_doc` is a buffer at all. */
        for (int k = 0; k < f->dyn_n; k++)
            if (f->dyn_doc[k] == doc) n++;
    }
    return n;
}
