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
#include "check.h"
#include <stdlib.h>

static Flow **g_flows = NULL;
static int g_flows_n = 0, g_flows_cap = 0;
static unsigned g_gen = 0;   /* bumped whenever the frontier's membership changes (add/remove) — lets the
                                value-yield recompute the rival only on a change, never per-opcode */
static Flow *g_running = NULL;   /* the flow currently holding the worker (the scheduler sets it) */

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
    g_running->val += v;
    g_running->cpu = 0;   /* emitted -> no longer a CPU-burning monopolizer */
    frontier_rank_changed();   /* rank changed: re-rank at this flow's next opcode */
}

/* Age the running flow by the MICROSECONDS of thread time its step just burned. A monopolizer that runs without
   emitting sinks below productive + unrun flows — see FLOW_AGE_RATE for the exchange that makes that true.
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

void flow_registry_free(JSContext *ctx) {
    /* THE WORLD NAMESPACE GOES DOWN WITH THE FRONTIER, for the reason it came up with it. Any segment this
       instance still holds for a PEER's world is a foreign flow's state in this document — nothing else is
       going to free it, and it is malloc'd, so the runtime's GC walk would never name it. */
    world_registry_free(ctx);
    for (int i = 0; i < g_flows_n; i++) {
        Flow *f = g_flows[i];
        /* A FLOW LEFT IN THE FRONTIER IS THE ORDINARY CASE, NOT AN ERROR — and everything it is still holding
           has to be released HERE, because nothing else will. The session closes over its survivors by design
           (engine_sched_step's exhausted path leaves every host-owed flow alive, and in the product a parked
           flow OUTLIVES the session entirely), so this is the teardown for a flow that never finished and it
           must release exactly what flow_finish releases for one that did.
           It released four fields and left the rest, and the two that matter are not small: `frame` is the
           JS_FlowNew handle holding this flow's whole heap-frame chain — every activation, closure and local
           it is suspended across — and `delta` is the COW state naming every shared value it captured. One
           survivor therefore retained the entire realm, and the runtime's leak walk reported it as a Window, a
           context and seventeen hundred anonymous Functions with nothing naming the owner. That is precisely
           the shape a leak takes when the ROOT is one dropped handle. */
        /* A PAGED FLOW IS NOT A DROPPED ONE, which is the only thing this assert is about. A continuation left
           parked here is an async activation nobody will ever resume — UNLESS the frontier was written to the
           cold tier, in which case this flow's recipe replays the code that created that continuation and
           re-parks it in the next session. That is the whole claim the cold tier makes, so it is stated where
           the drop would otherwise be asserted rather than by teaching the assert to tolerate a NULL. */
        DCHECK(engine_frontier_paged() || f->park_fn == NULL,
               "a flow reached the frontier's teardown with a continuation still parked — its suspended async "
               "activation is dropped, and only the flow that FINISHES was being checked for this");
        if (f->frame) { JS_FlowFree(ctx, (JSValue *)f->frame); f->frame = NULL; }
        /* the REPLIES it was still owed. Each entry is a request the host never answered; the whole register
           is one JS value now, so its resolve capabilities, its reply values and every string in it go with
           one release — which is the difference between a record whose free path had to be kept in step with
           its fields and one that cannot be. */
        pending_free(ctx, &f->pending);
        cow_delta_free(ctx, (CowDelta *)f->delta); f->delta = NULL;
        if (f->dom) dom_buf_free(f->dom, f->dom_n);
        if (f->dom_base) dom_base_free(f->dom_base);
        decide_blob_free(f->dec_blob);
        concolic_pins_blob_free(f->pin_blob);
        for (int k = 0; k < f->dyn_n; k++) free(f->dyn[k]);
        free(f->dyn); free(f->dyn_cand);
        free(f->cand_src); free(f->cand_payload);
        JS_FreeValue(ctx, f->fn);
        free(f->deliver); free(f->deliver_origin);
        for (int k = 0; k < f->njob; k++) {   /* free any undrained microtask jobs */
            for (int a = 0; a < f->jobs[k].argc; a++) JS_FreeValue(ctx, f->jobs[k].argv[a]);
            free(f->jobs[k].argv);
        }
        free(f->jobs);
        free(f);
    }
    free(g_flows); g_flows = NULL; g_flows_n = g_flows_cap = 0;
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
        DCHECK(JS_TrampFrameCount(rt) == 0,
               "the frontier is gone and heap CALL FRAMES are still live — a suspended chain that no flow owns "
               "is unreachable memory, and every frame of it holds its locals, its closed cells and its callee");
        DCHECK(JS_StepMachineCount(rt) == 0,
               "the frontier is gone and STEP MACHINES are still live — a continuation-holding builtin's state "
               "outlived the flow that was suspended inside it, along with every argument it captured");
    }
    /* AND THE DECISION STATE THE FRONTIER STANDS ON, released HERE and not by each host. Every flow's parked
       vector is a reference on a shared frozen chain, and the running flow holds one more in decide.c's
       globals; the blobs went with the flows in the loop above, so this is the last of them. Putting it in the
       three hosts' teardowns instead would be the hand-copied list build.mjs warns about — a host that forgot
       the line would leak the whole chain with nothing to say so, which is exactly how the world registry's
       own release came to be missing from one of them. It belongs to the frontier, so it goes down with it. */
    decide_free();
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
        g_flows_cap = g_flows_cap ? g_flows_cap * 2 : 32;
        g_flows = realloc(g_flows, (size_t)g_flows_cap * sizeof(Flow *));
        CHECK(g_flows, "flow_add: OOM growing the frontier — a dropped flow corrupts BFS exploration");
    }
    Flow *f = calloc(1, sizeof(Flow));
    CHECK(f, "flow_add: OOM allocating a flow — a dropped flow corrupts the frontier");
    f->fn = JS_DupValue(ctx, fn);
    /* THE PENDING REGISTER IS EMPTY, AND EMPTY IS NOT AN ARRAY. Most flows never park on anything, so
       allocating one per flow would put a JSObject on the heap for every member of a frontier that reached
       29550 on this fixture — and the blocked scan the preempt hook runs at every suspend point would then
       have to read a length instead of testing a tag. It is minted by the first push. */
    f->pending = JS_UNDEFINED;
    /* …and the register's own context, named HERE because this is the one point every flow passes through.
       Putting it in each host's setup would be the hand-copied list build.mjs warns about. */
    pending_set_ctx(ctx);
    f->val = 0.0; f->cpu = 0;
    f->cand_src = NULL; f->cand_payload = NULL; f->cand_sink = NULL;
    f->last_compiled = -1;   /* nothing compiled yet; see the no-replay DCHECK at the compile site */
    f->world = w;
    g_flows[g_flows_n++] = f;
    frontier_rank_changed();   /* frontier changed: the newcomer may outrank the flow holding the thread */
    return f;
}

Flow *flow_add(JSContext *ctx, JSValueConst fn, WorldId parent) {
    /* THE WORLD IS MINTED HERE so no flow can exist without one. A fork passes its parent's world and the
       child records the edge, which is what lets another instance materialize this flow's segment by forking
       the nearest ancestor it already holds. A from-baseline flow passes WORLD_NONE and gets a root. */
    return flow_new(ctx, fn, world_is_none(parent) ? world_mint() : world_mint_child(parent));
}

/* BLOCKED = holding an unanswered synchronous host request. Scanned rather than counted because a counter is
   a second representation of the same fact, and the two drift at exactly the sites (fork, drain, release) that
   are already the hardest to keep in step. The register is short — it is one flow's outstanding requests. */
int flow_blocked(const Flow *f) { return pending_blocked(f->pending); }

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
 * That crossing is not left as prose — flow_best asserts it below, and it FIRES on the tree this replaced. */
#define FLOW_AGE_RATE 1e-6

/* Anytime-bandit priority: reward + UCB optimism − CPU aging. Additive (never a value/cpu ratio — the aging
   term already yields value-per-CPU behaviour without the 0/0 degeneracy on an unrun flow). */
double flow_weight(const Flow *f) {
    double reward = f->val;
    /* OPTIMISM DECAYS WITH SERVICE, NOT WITH BEING PICKED. Keyed on the DISPATCH COUNT it fell by 0.167 the first time the
       scheduler chose a flow — forty times a quantum of aging — so the act of switching to a flow was enough to
       make it no longer the best one, and the scheduler switched straight back. The guarantee is unchanged and
       is what the term is for: a flow that has never RUN has no service, so it carries the full bonus and
       cannot be starved. What is gone is a rank that moves because of a decision rather than because of work.
       ZERO SERVICE IS ITS OWN NOTCH, and that is the whole of the guarantee rather than a rounding detail.
       Quantising with a FLOOR put a flow that has never run and a flow that has burned a quantum-minus-one of
       CPU into the SAME notch, so they tied — and a tie does not dislodge the running flow, because the value
       yield asks whether a rival's weight is STRICTLY greater (engine.c's preempt hook). "A never-run flow is
       never starved" is then not a guarantee at all: for a whole service quantum the term cannot tell the two
       apart, and the one already holding the thread wins. A CEILING restores it and costs the anti-thrash
       property nothing: service 0 is the never-run notch and is strictly better than every serviced flow, while
       everything up to one quantum lands on notch 1 and ties with itself exactly as it did, so two flows that
       have both run still hold the thread for a whole quantum and trade places once, never per step. Measured on
       the full smoke fixture: same PASS, 13796 flows against 14003, 37002 context switches against 14000 — the
       extra switches are fresh forks taking their turn, which is what the term is for. */
    int64_t served = (f->cpu + FLOW_SERVICE_US - 1) / FLOW_SERVICE_US;   /* 0 IFF never serviced */
    double ucb     = 1.0 / (1.0 + (double)served);
    /* THE AGING, IN THE SAME CURRENCY AS THE REWARD ABOVE — see FLOW_AGE_RATE. Quantised to the same notch the
       optimism term uses, so a flow's weight is constant between two service quanta, which is also what lets the
       preempt hook's cached rival stay exact at O(1) (engine.c). */
    double aging   = (double)(f->cpu / FLOW_SERVICE_US) * ((double)FLOW_SERVICE_US * FLOW_AGE_RATE);
    return reward + ucb - aging;
}

/* THE CPU AT WHICH A FLOW IS GUARANTEED TO RANK BELOW ANY NEVER-RUN SIBLING — the crossing §scheduler's sentence
   claims exists, computed from the flow's OWN reward rather than a fixed threshold that could fire falsely on a
   very productive flow. A never-run flow carries the full optimism bonus and no aging, so its weight is its
   reward + 1.0 and therefore at least 1.0; a serviced flow's is at most val + 0.5 − aging, and aging is at least
   (cpu − one quantum) * RATE. Past this charge the second is below −0.5 and the comparison cannot go the other
   way for any reward. `static inline` so release (where the DCHECK's condition is unevaluated) does not warn. */
static inline int64_t flow_cpu_to_sink(const Flow *f) {
    return (int64_t)((f->val + 1.0) / FLOW_AGE_RATE) + FLOW_SERVICE_US;
}

Flow *flow_best(void) {
    Flow *best = NULL; double bw = 0.0;
    /* A MEMBER WITH ZERO SERVICE: never run, or just emitted (flow_credit_emit zeroes cpu). Either way it
       carries the full optimism bonus and no aging, so its weight is its reward + 1.0 and hence at least 1.0 —
       which is the only property the assertion below needs, and it is the same property for both. */
    int unrun = 0;
    for (int i = 0; i < g_flows_n; i++) {
        double w = flow_weight(g_flows[i]);
        if (g_flows[i]->cpu == 0) unrun = 1;
        if (!best || w > bw) { best = g_flows[i]; bw = w; }
    }
    /* §scheduler'S SENTENCE, ASSERTED WHERE THE CHOICE IS MADE — "CPU-AGING so a monopolizer that burns CPU
       without emitting sinks below productive+unrun flows". This is the one line in the engine that decides
       which flow runs, so it is where the claim is either true or a comment. It is not a tautology dressed as a
       check: it FIRES on the tree this replaced, where the charge was one unit per scheduler step and the walk
       above was re-picked at every iteration for 227 seconds with unrun siblings waiting. It fires again on any
       future edit that clamps the aging term, caps it, or lets the reward grow with the CPU it is weighed
       against — which is exactly the shape a "fix" for the thrash this quantum handles would take. */
    DCHECK(!best || !unrun || best->cpu < flow_cpu_to_sink(best),
           "the WFQ re-picked a flow that has burned more thread time since its last emit than its entire "
           "accumulated reward is worth, while a never-run flow was waiting — the aging term is no longer "
           "commensurate with the reward it is subtracted from, so a monopolizer cannot sink");
    return best;
}

/* The highest-weight flow OTHER than `exclude` — the running flow's rival for the value-driven yield. */
Flow *flow_best_other(const Flow *exclude) {
    Flow *best = NULL; double bw = 0.0;
    for (int i = 0; i < g_flows_n; i++) {
        if (g_flows[i] == exclude) continue;
        double w = flow_weight(g_flows[i]);
        if (!best || w > bw) { best = g_flows[i]; bw = w; }
    }
    return best;
}

void flow_remove(JSContext *ctx, Flow *f) {
    /* WHAT THIS FUNCTION DOES NOT FREE, ASSERTED RATHER THAN ASSUMED. A Flow owns fourteen things and this
       releases five; the other nine belong to the caller's teardown (engine.c's flow_finish), which runs while
       the flow is switched IN because releasing a COW delta or a DOM head means unapplying it from the live
       heap and document first. That split is correct and it is also invisible: nothing said so, so a second
       caller — an eviction, a cancelled candidate, a frontier trim — would compile, run, and leak the flow's
       entire delta, its DOM head, its queued jobs and its suspended frame with no counter naming the owner.
       That is precisely the shape the delta leak took (a per-flow allocation nobody freed, holding no live
       JSValue by then, so the runtime's own leak walk could not see it either).
       Asserted at the ROOT of the contract instead: a field added to Flow gets a line here, and a caller that
       has not run flow_finish crashes at the removal instead of two hundred megabytes later. */
    DCHECK(f->delta == NULL && f->dom == NULL && f->dom_base == NULL,
           "a flow was removed with its COW state still attached — the heap delta and the DOM head must be "
           "UNAPPLIED from the live heap and document before they can be freed, which is why flow_finish owns "
           "that and this does not: removing it here drops both");
    DCHECK(f->frame == NULL && f->park_fn == NULL,
           "a flow was removed holding a live frame or a parked continuation — its whole activation chain, and "
           "everything those frames close over, would be retained by a handle nothing will ever free");
    DCHECK(f->njob == 0 && f->jobs == NULL && JS_IsUndefined(f->pending),
           "a flow was removed still holding queued jobs or pending host replies — each is a work item on the "
           "one frontier, and the WFQ may never drop one");
    DCHECK(f->dyn == NULL && f->dyn_cand == NULL && f->dec_blob == NULL && f->pin_blob == NULL,
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
