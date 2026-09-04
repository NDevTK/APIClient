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
static long g_rank_changes = 0;   /* …and the same event counted for the LIFE of the instance, so it is
                                     commensurable with the scan counters, which nothing resets either.
                                     See frontier_rank_changed for why the generation NUMBER is not. */
static unsigned g_gen = 0;   /* bumped whenever the frontier's membership changes (add/remove) — lets the
                                value-yield recompute the rival only on a change, never per-opcode */
/* EVERY DISPATCH THIS INSTANCE HAS EVER MADE — a LIFETIME COUNTER, and it is here rather than on a Flow
   because that is the only place it can be one. `Flow.picks` is a GAUGE the moment a member departs: the
   number leaves the frontier with it, so a sum over the members standing NOW is a statement about the live
   population and can FALL between two censuses. This cannot; it only rises, so it is the one of the two that
   may be differenced across samples, and the pair is what separates "each dispatch reached a fresh member"
   from "the dispatches are landing on members that already had one" — which no row in this file could ask.
   NOT RESET BY flow_registry_init, for the reason stated at `g_rank_changes` two lines up and at
   frontier_rank_changed: a quantity a second registry would zero is not commensurable with the scan counters
   nothing zeroes, and a reader dividing one lifetime by another lifetime-that-restarted gets a ratio about no
   run at all. CROSS-CHECKABLE, which is what makes it a counter rather than a number: flow_credit_pick has
   exactly ONE caller and engine.c increments its own `g_switches` on the line beside it, so this must equal
   the result document's `_switches` for the same instance, and a divergence names a second dispatch path. */
static int64_t g_picks_total = 0;
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
       WHY IT REPLACED THE PER-ARM CHARGE. The reward was then COPIED down every arm at its fork, so a family
       that emitted V and had N live arms presented V exactly N times; the aging that is supposed to cancel V was
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
       SO THE TWO TERMS ARE NOW SYMMETRIC: the reward is the FAMILY's (`val` below) and the aging that cancels
       it is the FAMILY's (charged once however many names the family wears). §scheduler's sentence — "a
       monopolizer that burns CPU without emitting sinks below productive+unrun flows" — is about a monopolizer,
       and this file already said which one: "a fork chain is one monopolizer wearing N names, so it enters at
       one place and it is charged as one thing." It entered at one place and was charged as N.
       THAT SENTENCE USED TO READ "the reward is the FAMILY's (COPIED AT EVERY FORK)", AND THE PARENTHESIS
       REFUTED THE CLAIM IT WAS OFFERED AS EVIDENCE FOR. A quantity copied at the fork INSTANT is not the
       family's: two arms of one parent, forked at two instants, hold what the parent held at two different
       times, so they differ by exactly what the parent emitted in between — which neither of them did. The
       reward was therefore a PER-CHAIN PREFIX wearing a family's name, and the aging that was charged once per
       family could not cancel it, because a common offset cancels out of every within-family comparison while
       the prefix spread does not. On a real page the whole frontier is ONE family, so that spread WAS the
       order: members served strictly newest-first, the tail unreachable at any silence, and a frontier that
       grows without draining. The field below is what makes the sentence true. */
    int64_t fam_us;
    /* THE FAMILY'S ACCUMULATED EMITTED VALUE — §scheduler's reward term, held at the SAME accounting unit that
       pays the aging above and for the same reason, MEANINGFUL ONLY ON A ROOT.
       A CREDIT MINTED ONCE MAY BE PRESENTED ONCE. The defect the paragraph above describes is one emission
       raising N members' rank while the debit that cancels it is charged to one account: the credit inflates
       by the fork factor of the page's own bundle and the debit does not, so no amount of silence can ever pay
       it off, and §scheduler's "a never-run flow is never starved" is false of every frontier whose reward
       spread exceeds the optimism term's whole range of 1.0. Held here, one emission raises the family once,
       and the silence charged to that family cancels exactly it.
       IT IS ALSO WHAT MAKES A FORK RANK-NEUTRAL FOR FREE, AND ACROSS MORE THAN ONE INSTANT. A copied term is
       neutral only between a parent and the arm born at that instant; flow_fork_inherit's equality is written
       over one branch and cannot see the pair it does not compare — two arms of the same parent, forked at two
       instants, which must also be worth the same because neither did anything between the two branches. A
       term read through the shared root satisfies both by construction: there is nothing to copy, so there is
       nothing a fork can carry differently.
       WHAT THE ORDER IS THEN MADE OF, AT TWO SCOPES, WHICH IS THE POINT AND NOT A SIDE EFFECT. WITHIN a family
       this term is a common offset and cancels, so members are ordered by the optimism bonus and their OWN
       silence — both bounded, and NEITHER of them reset by the emitting MEMBER, which is the correction this
       sentence carries and which took two diffs to become true of both halves. The optimism bonus is not moved
       by an emission at all (flow_credit_emit says why the per-member zero that used to move it was one credit
       presented twice). The own silence is forgiven for every arm of the ACCOUNT at any arm's emission
       (`emit_gen` below) EXCEPT the emitter, which carries its burn across the bump for that same reason: the
       credit is the family's and the debit is the member's, so erasing the second with the first is one credit
       presented twice, and it erased the only within-family ordering signal there is. So what orders a
       family's members is fewest completed units first and least own burn first — fair queueing over the arms and
       §scheduler's BFS. BETWEEN families it is the whole bandit ordering, charged once against a silence
       charged once, so a family that emitted V holds the thread for V seconds of ITS OWN silence and is then
       passed. Neither scope can do the other's job, which is the same sentence flow_silence_notch already
       makes about the two halves of the aging. */
    /* …AND IT IS TWO QUANTITIES, WHICH ONE FIELD HID UNTIL AN ASSERT WENT LOOKING FOR THE OTHER. The reward
       this account is ranked by is `base + earned` and always was; what one field could not say is WHICH of the
       two any given point came from, and they are not the same kind of thing at all.
       `earned` IS A LEDGER: what THIS account has emitted, raised only by flow_credit_emit, one point per
       finding, unbounded by §NO BOUNDS because bounding it would cap how much a family may be worth.
       `base` IS A QUEUE COORDINATE: where this account ENTERED the order. A from-baseline flow founds its own
       family and must not enter below the whole frontier, so flow_arrive_at_virtual_time places it at the
       virtual time of the account in service; a resumed flow is placed at the total the session that parked it
       wrote down. Neither is a thing this account emitted, and nothing about either says it did.
       WHAT THE CONFLATION COST, AND IT IS NOT HYPOTHETICAL — IT WAS AN ASSERT THAT WOULD HAVE FIRED ON A
       HEALTHY FRONTIER. The census asserts that an account's forgiveness count (`emit_gen`) is past zero
       exactly when its reward is, because flow_credit_emit raises both in one statement with no return between
       them. That is TRUE of `earned` and FALSE of the sum: an arrived account stands at a reward of whatever
       the leader held — 189 points, measured — with `emit_gen` still at 0, because arriving is not emitting.
       The assert had not fired only because such an account had never reached the FRONT of the order, which is
       precisely the starvation the split is a step toward fixing: the first diff that let a candidate session
       lead would have aborted the engine on an invariant that was right about the ledger and wrong about the
       field.
       AND IT IS THE SEAM THE REWARD BAND NEEDS. `val_min` pinned across a whole run while `val_max` gains two
       hundred is an account sitting at the coordinate it ARRIVED at while another EARNS past it, and no term
       can re-relate the first without naming it apart from the second — and `placed` one field down is that
       re-relation, which is why the split had to land before it could. Split, that is one field to re-derive;
       summed, it was a ledger nobody may touch. The order reads `base + earned` through acct_family_val and is
       unchanged by this split, which is the point: the arithmetic is identical and the QUESTIONS are separable.
       `self_emit` and `val_zero` (flow.h) are the census rows that were quietly answering about the sum. */
    double base;
    double earned;
    /* …AND WHETHER `base` IS A STORED TAG AT ALL, WHICH IS THE OTHER HALF OF THE SPLIT ABOVE AND THE THING THAT
       MAKES THE COORDINATE A RELATION RATHER THAN A COPY. `base` above says this account entered the order at
       the frontier's virtual time; what it could not say is WHEN that reading was taken, and a reading taken
       ONCE, at creation, is stale from the instant the account in service earns past it.
       ZERO MEANS THE ACCOUNT HAS NEVER HELD THE THREAD, and while it is zero `base` is not read at all: the
       account's coordinate IS the frontier's virtual time (g_vt), re-read at every pick. ONE means the account
       has been dispatched at least once, and from that dispatch onward `base` is the tag frozen at the virtual
       time it was standing at — SFQ's F, taken when the item is first served. So an account that has been
       given nothing does not fall behind, and an account that HAS been served stands on its own tag plus its
       own ledger, which is what keeps `earned` an ordering between accounts instead of a number the arrival
       rule erases.
       WHY THE FREEZE IS AT SERVICE AND NOT AT CREATION, WHICH IS THE WHOLE OF THIS FIELD. Start-time fair
       queueing's `max(v(t), F_prev)` is evaluated at each ARRIVAL, and its content is that a member which has
       not been SERVED does not fall behind the clock — in SFQ that holds for free, because the server takes the
       MINIMUM tag and v(t) can therefore never pass a waiting member. This order takes the MAXIMUM and an
       emission RAISES the served account's tag, so v(t) runs AWAY from the waiting members instead of toward
       them, and a coordinate stamped at creation is passed by exactly what the incumbent earns afterwards.
       Measured, on the build smoke fixture: `valMin` pinned at 183.0 from the second census to the last while
       `valMax` gained 217 and the frontier grew to 660 members, with `valArrived` — accounts standing entirely
       on the coordinate they arrived at — frozen at 12 beside `cands: 12`, `turns: 0` on every one of them.
       Those twelve were the @S candidate sessions, so every §@S mechanism was unreachable by construction.
       AND IT MUST BE A FACT ABOUT THE ACCOUNT, NEVER ABOUT A MEMBER. Two arms of one parent forked at two
       instants must read the coordinate the same, so there is nothing here for a fork to carry differently:
       the arm joins the family (flow_fork_inherit) and reads this bit through the same pointer. A per-flow
       copy of it would be reset-by-splitting arriving through the arrival door. */
    unsigned char placed;
    /* WHICH SILENCE WINDOW THIS ACCOUNT IS CURRENTLY IN — bumped by every emission credited here, and read by
       nothing except `Flow.cpu_gen` beside it. It is the OTHER half of the reset `fam_us` above performs, and
       it exists because the aging term has two halves that were measured over TWO DIFFERENT WINDOWS while being
       SUMMED into one notch and weighed against ONE reward.
       WHY THAT IS A UNIT ERROR AND NOT A POLICY. `fam_us` is the family's burn since ANY arm of it last emitted;
       `Flow.cpu` was this flow's burn since ITS OWN last emission, which is a strictly LONGER window, because a
       flow's own emission is also one of its family's. The reward those two are subtracted from is the FAMILY's
       (`val` above), credited over the family's window — so the own half was a debt denominated in a window the
       income could not cover, and for one population it could never be retired at all: `Flow.cpu` is written
       only for the flow HOLDING THE THREAD, so a member the scheduler has never dispatched carries whatever its
       parent had burned at the fork, forever, while the parent's is forgiven at the parent's next emission.
       The arm is then permanently behind its own parent by `floor(parent_cpu_at_fork / S) * FLOW_AGE_QUANTUM`,
       for thread time the arm never consumed, repayable only by being dispatched — which is the thing it is
       being denied. That is §scheduler's razor's STARVES: not a deprioritisation, a debt whose only currency is
       the dispatch it forecloses.
       MEASURED, on the build smoke fixture, over the run's 71 censuses: `unrun` — the count of members standing
       at `cpu == 0` — was ZERO at 69 of them, nonzero only at censuses 8 and 9 while the frontier was under a
       thousand members and never again after it passed one; `neverPicked`
       reached 943 with the best of them 0.011 from the front of the order, and sat pinned at ~819 across a
       dozen consecutive censuses in which the frontier grew by 437 and every one of those newcomers was
       dispatched. Because `unrun` is also the population flow_pick's two ordering guards are gated on, both of
       them short-circuited to vacuity at every pick of the entire run.
       SO THE OWN HALF IS READ OVER THE FAMILY'S WINDOW TOO, which is what this generation makes O(1). A member
       whose `cpu_gen` does not match this counter has burned nothing since the window opened, so its own
       silence IS zero and no walk of the family is needed to say so. The two halves are then one quantity read
       at two scopes — which is what flow_silence_notch already claimed of them — rather than two clocks with
       two epochs.
       uint64 and never wrapped: one increment per emitted finding cannot reach 2^64 in any session, and a wrap
       would alias a stale mark onto the live generation, which reads a frozen silence as a current one and
       restores the exact defect this field removes. */
    uint64_t emit_gen;
    /* THE CENSUS'S MARK, AND IT IS HERE BECAUSE A FAMILY HAS NO MEMBER OF ITS OWN TO BE COUNTED THROUGH. The
       census reaches families only via their arms, so counting DISTINCT ones out of one walk of the frontier
       is a set problem, and the set's identity is this NODE'S ADDRESS rather than any value it holds — two
       families standing at the same `fam_us` are two families, which is exactly the state a pair of extrema
       cannot see. Marking the root as the walk passes it makes the count O(members) with one word per node and
       no ancestry walk, which is the shape a renderer counts distinct objects in one traversal by.
       ZERO IS THE UNMARKED STATE and `reclaim_calloc` supplies it, so a node minted between two censuses is
       counted by the next one instead of inheriting a stale mark; the generation is bumped past zero on wrap
       for the same reason. It is written only by the census, which decides nothing. */
    unsigned census_gen;
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

/* …AND THIS ONE FLOW'S OWN SHARE OF THAT SAME WINDOW — the aging term's other half, read over the family's
   silence window rather than over the flow's own, for the reason FlowAcct's `emit_gen` gives. `Flow.cpu` is a
   reading only while `cpu_gen` matches the account's generation; past an emission the field holds a stale
   window's arithmetic and the answer is ZERO, because a flow that has not been charged since the window opened
   has burned nothing in it.
   IT IS THE ONE PLACE `Flow.cpu` MAY BE READ AS A QUANTITY. Every other site — the weight, both notches, the
   census rows, flow_pick's guard, the arrival rule — goes through here, so the field and its generation cannot
   be separated by an edit that touches only one of them. Reading the raw field instead is exactly the defect:
   it charges a member for thread time it never consumed and that no emission of its family can forgive.
   READ-ONLY, with no lazy normalisation, because flow_weight calls this once per member per pick and
   flow_weight is evaluated inside DCHECK conditions — a write here would make every assertion in this file
   mutate the structure it is asking about. The normalisation happens where the flow is CHARGED
   (flow_age_running), which is a write site already, and it is observationally a no-op: it writes the field to
   the value this function was already returning for it. */
static int64_t flow_own_silence(const Flow *f) {
    if (!f->family) return 0;                   /* departed: its node is gone and so is its rank */
    return f->cpu_gen == f->family->emit_gen ? f->cpu : 0;
}

/* THE ONE RELATION THE AGING'S TWO HALVES OWE EACH OTHER, AND THE ONLY THING THAT MAKES THEM SUMMABLE: a
   flow's own burn over the window is part of its FAMILY's burn over that same window, so the own half can
   never exceed the family half. The notch adds them and weighs the sum against ONE reward (the family's), and
   that addition is only meaningful while both readings are over one epoch — which is a claim about the two
   fields' WINDOWS, not about their magnitudes, and it is a claim nothing else in this file states.
   IT IS THE EXACT SHAPE OF THE DEFECT IT EXISTS TO FORBID, which is why it earns a check rather than a
   comment. The own half used to be measured since this FLOW's own last emission and the family half since ANY
   arm's — a strictly longer window against a strictly shorter one — and the observable of that mismatch is
   precisely this inequality inverting: a sibling forked BEFORE its family emitted kept the raw burn it had
   inherited, while the emission zeroed the family's, so the arm stood at `own > 0 == fam` and paid a debt no
   emission of its family could forgive and no dispatch would ever be offered to clear. Measured, on the
   artifact built one commit before the repair and on this reproduction: a document whose gate forks a sibling
   and then forks an unbounded generator emitted ONE of its two worlds, and putting a longer busy loop between
   the two — which raises the `cpu` every newborn rival copies at its fork, and nothing else — brought the lost
   world back. An ordering that a rival's idle burn can restore is not one any resume seam can be blamed for.
   ASSERTED AT EVERY TRANSITION THAT CAN MOVE EITHER HALF AND NOWHERE ELSE — a MACRO expanded at each of them,
   never a function they call, so the abort stamps the transition that broke the relation instead of naming one
   forwarding line for all four. It is deliberately NOT placed in flow_own_silence: that reader is called once
   per member per pick and from inside other DCHECK conditions, so a check there would report the same line for
   every caller and would name no site to fix, which is the assert-with-a-remedy-and-no-address shape.
   The condition is READ-ONLY on both sides (both readers say so at their own definitions), so it is a legal
   DCHECK condition and evaluating it cannot move the structure it asks about. */
#define DCHECK_AGING_ONE_WINDOW(f, what)                                                                     \
    DCHECK(flow_own_silence(f) <= acct_family_us(f),                                                          \
           "a flow's OWN silence exceeds its FAMILY's over the same window, after " what " — the aging term "  \
           "SUMS the two and weighs them against the family's single reward, so this says they are being "     \
           "read over two different epochs and the sum is a unit error. The arm now carries a debt its "       \
           "family's next emission cannot forgive, repayable only by the dispatch the deficit itself "         \
           "forecloses, which is §scheduler's razor's STARVES: fix the transition this abort names so both "   \
           "halves move over the epoch FlowAcct's `emit_gen` keeps")

/* THE QUEUE COORDINATE, forward-declared because the frontier's virtual time below is a reading OF it and the
   ordering itself is stated once, further down beside the terms it is made of. */
static double flow_queue_weight(const Flow *f);

/* …AND THE REWARD ALONE, forward-declared because the clock's writer samples THAT and not the queue weight —
   see frontier_vt_serve for why the server's own aging may not enter the coordinate a newcomer arrives at. */
static double acct_family_val(const Flow *f);

/* …AND THE ONE STATEMENT THAT SAYS THE ORDER MOVED, forward-declared because the clock's writer below raises
   it and the raise is defined with the registry it belongs to. */
static void frontier_rank_changed(void);

/* THE FRONTIER'S VIRTUAL TIME — SFQ's v(t), WHICH IS THE SERVICE TAG OF THE ITEM IN SERVICE AND NOT A QUANTITY
 * OF ITS OWN. It is the one place the queue's clock is spelled, and the reason it is a stored scalar rather
 * than a derivation is that it has to be readable when NOTHING is in service: the frontier persists across
 * slices and across sessions, so a definition that answers only while `g_running` is set would hand every
 * newcomer a different coordinate depending on whether the host happened to be between two dispatches.
 *
 * IT IS WRITTEN AT EXACTLY ONE EVENT — AN EMISSION BY THE ITEM IN SERVICE, which raises that item's
 * coordinate by what it just earned, so v(t) follows it up. Between two emissions the clock is CONSTANT, which
 * is what SFQ says of it and what makes a newcomer's placement a fact rather than a race with whichever
 * microsecond it was created in.
 *
 * IT USED TO BE WRITTEN AT TWO, AND THE SECOND ONE WAS A DISPATCH. That is deleted rather than described,
 * because it was not a second flavour of the same event: a dispatch moved this clock to the WINNER's own
 * queue coordinate, and an unplaced account's reward IS this clock read live — so the act of choosing a flow
 * lifted every account the pick had just compared it against to that flow's own coordinate, where a full
 * optimism bonus then put them above it. The pick un-chose its own winner, every time, and engine.c's
 * value-yield assertion fired on a state whose five clauses are all terms of the winner and so could not see
 * it. See flow_set_running for the arithmetic and for why the argument in this banner was only ever about the
 * emission. What survives is the property that argument actually needs: the clock follows what the frontier
 * EARNS, and never which member happens to be holding the thread.
 *
 * AND EVERY WRITE OF IT RAISES THE FRONTIER GENERATION, at the writer rather than at the caller — it is a term
 * of every unplaced account's weight, so a write re-ranks all of them, and the preempt hook's rival cache is
 * correct only if such a move arrives through the generation. A second caller therefore cannot be added
 * without one, which is exactly how the dispatch came to move this clock silently.
 *
 * AND IT DELIBERATELY DOES **NOT** FOLLOW THE AGING DOWN, WHICH IS THE ASYMMETRY THE WHOLE FIX RESTS ON. The
 * item in service sinks as it burns the thread, and following it down would charge every member that has never
 * been served for the thread time of the one that IS being served — the unforgivable debt this file already
 * removed from the arrival door in the aging term, arriving through the clock instead. A member that has been
 * given nothing owes nothing.
 *
 * FLOORED AT ZERO, WHICH IS THE CLOCK'S ORIGIN AND NOT A CLAMP ON A COMPUTED VALUE. The frontier's first flow
 * enters at v(0) = 0 and no member has ever entered below that, so a serving tag below zero — an item that has
 * burned more than its whole reward is worth, which is exactly the state flow_pick's monopolizer guard names —
 * moves the clock no further back than the start of the busy period. The floor is also what keeps that guard's
 * own derivation true: it reads `val + 1` off the picked flow and off the untouched member it is compared
 * against, and a negative reward on a member that has done nothing would make the bound fire on a healthy run.
 *
 * READ-ONLY AND O(1) AT EVERY READER, because acct_family_val is called once per member per pick and from
 * inside DCHECK conditions, where a reader that walked or normalised anything would make every assertion in
 * this file mutate the structure it is asking about. */
static double g_vt;

static double frontier_vt(void) {
    DCHECK(g_vt >= 0.0 && g_vt == g_vt,
           "the frontier's virtual time is not a coordinate any member could have entered at — the clock is "
           "floored at the start of the busy period where it is written, so a negative or NaN reading here is "
           "a second writer that did not go through frontier_vt_serve, and every unplaced account is being "
           "ranked at a position no flow has ever stood at");
    return g_vt;
}

/* …AND THE ONE WRITER OF IT. `f` is the item whose tag the clock is now a reading of: the flow just dispatched,
   or the flow in service whose own tag an emission has just raised. Called with the flow ALREADY installed as
   `g_running`, so the coordinate it reads is the one every other member is about to be compared against. */
/* DOES THE CLOCK STILL LEAD EVERY ACCOUNT — the definition of `g_vt` asked as a question, for the assertion in
   the writer below. Walked over the MEMBERS because an account is reached through its arms, exactly as the
   census counts families; a family reached twice answers the same value twice, so no marking is needed for a
   maximum. Mentioned only inside a DCHECK condition, so release neither calls it nor emits it — the shape
   flow_is_min_weight already establishes in this file. */
static int acct_vt_leads(void) {
    int i;
    for (i = 0; i < g_flows_n; i++)
        if (g_flows[i]->family && acct_family_val(g_flows[i]) > g_vt) return 0;
    return 1;
}

static void frontier_vt_serve(const Flow *f) {
    double v;
    DCHECK(f != NULL && f->family != NULL,
           "the frontier's clock was asked to follow a flow that owns no account — a departed flow cannot be "
           "the item in service, and taking its coordinate would drop the clock to zero and bury every member "
           "that has never been served underneath the whole frontier");
    DCHECK(f->family->placed,
           "the frontier's clock was asked to follow an account that is still reading the clock — the two "
           "would define each other, so this is a dispatch that installed a flow without stamping its account "
           "first (see flow_set_running, which does both in one operation and in that order)");
    /* THE REWARD AND NOT THE QUEUE WEIGHT, WHICH IS THE WHOLE OF WHAT THIS SAMPLE IS. It read
       flow_queue_weight, and that carries the server's own AGING — negative, and grown by the whole FAMILY's
       thread time — so the coordinate every newcomer arrived at was the leader's earnings MINUS the leader's
       debt. That is not a statement anybody can act on: a newcomer owes nothing (FlowAcct's `placed`: "a
       member that has been given nothing owes nothing"), so charging it for the leader's silence is the
       unforgivable-debt shape this file already removed from the arrival door in the aging term, arriving
       through the clock instead. MEASURED: with the dispatch write gone, the monotonicity assert that stood
       here fired on the emission caller alone, which leaves the aging as the only term in the sample that can
       fall. The coordinate a newcomer arrives at is a statement about what the frontier has EARNED. */
    v = acct_family_val(f);
    v = v > 0.0 ? v : 0.0;      /* the clock's origin: no member has ever entered below the busy period's start */
    /* AND IT IS THE MAXIMUM OVER ACCOUNTS, TAKEN INCREMENTALLY — WHICH IS SFQ'S OWN `max(v(t), F)` COMPUTED
       ONCE HERE RATHER THAN AT EVERY ARRIVAL, AND IT IS NOT THE CLAMP THIS FUNCTION REFUSED ONE DIFF AGO.
       The difference is what the quantity DOES. `flow_queue_weight` genuinely falls — the aging is a real
       decrease of a real quantity — so a maximum over IT would have discarded a measurement, which is a `?:`
       past a broken invariant and was refused as one. `acct_family_val` is `base + earned` with `base` frozen
       at placement and `earned` a ledger nothing decreases, so NO ACCOUNT'S COORDINATE EVER FALLS. A lower
       sample here is therefore never a decrease of anything; it is a DIFFERENT, lesser account emitting, and
       the maximum is the only reading that answers the question the clock is asked — where does the FRONT of
       the order stand — rather than the question of who emitted last.
       AND THE INCREMENTAL FORM IS EXACT, NOT AN APPROXIMATION, which is what makes it a definition. Every
       increase of any account's `acct_family_val` passes through one of exactly three sites and this covers
       all three: an EMISSION (`earned += v`, which calls this function); a PLACEMENT (flow_set_running writes
       `base = frontier_vt()` and asserts `earned == 0`, so the value equals `g_vt` and cannot exceed it); and
       a cold-tier RESTORE (flow_restore_reward writes a parked coordinate that CAN lead, which is why it now
       calls this function too). An UNPLACED account reads `frontier_vt() + earned` with `earned == 0` forced
       by acct_family_val's own assertion, so it stands exactly at `g_vt`. There is no fourth writer, and the
       assertion below is what makes that a checked claim rather than this paragraph. */
    if (v > g_vt) g_vt = v;
    /* THE DEFINITION, ASSERTED — `g_vt` IS THE MAXIMUM COORDINATE ANY ACCOUNT STANDS AT. This replaces the
       monotonicity assert that stood here, and it is strictly stronger: a clock that only never decreases can
       still sit BELOW the leading account, at which point a newcomer arrives underneath a member it has every
       right to tie, `valMin` pins at a coordinate the frontier has left behind, and nothing says so. What it
       catches is a FOURTH writer of a reward — the one thing the paragraph above claims does not exist — and
       that is exactly the edit this is here for, because such a writer is invisible in every other row: the
       account leads the order, the clock does not know, and the two disagree silently for the rest of the
       session. O(members) and DEV-ONLY, in the shape flow_is_min_weight establishes in this file, and paid
       once per EMISSION rather than per pick — a finding is not a hot path, and the pick beside it already
       walks the frontier. */
    DCHECK(acct_vt_leads(), "an account stands at a reward ABOVE the frontier's clock — the clock is the "
           "coordinate every newcomer and every unplaced account is ranked at, so a leader the clock does not "
           "know about means arrivals are entering BELOW a member they are entitled to tie, permanently and "
           "with no row saying so. A reward was written by a site that did not serve the clock");
    /* AND MOVING THE CLOCK IS A RANK CHANGE, RAISED AT THE WRITER AND NOT AT THE CALLER. This value is a term
       of every unplaced account's weight, so a write to it re-ranks all of them at once; the preempt hook's
       rival cache is correct only if every such move arrives through the frontier generation, which is what
       engine.c means by "the eligible set the hook ranks against is the same one the pick used". Raised HERE
       so a second caller cannot be added without it — which is exactly how the dispatch caller came to move
       this clock silently, and the whole of the defect this file removed one diff ago. It is raised on every
       call and not only where the maximum moved: a caller reaching this function has just written a reward,
       and an account's own coordinate changing re-ranks that account whether or not it took the lead. */
    frontier_rank_changed();
}

/* …AND THE REWARD THAT SILENCE IS SUBTRACTED FROM, read through the SAME pointer for the same reason — see
   FlowAcct's `val`. It is the WFQ's reward term and the only reading of it the order makes: `Flow.val` beside
   it is what ONE member emitted and is a census quantity, never a rank. Exported as flow_reward (flow.h)
   because the node is private to this file and the ranked-state cache and the cold tier both have to name the
   quantity the weight is actually a function of. */
static double acct_family_val(const Flow *f) {
    /* THE ORDER'S REWARD IS THE SUM OF THE TWO, and this is the one place that sum is spelled — every
       reader in the tree comes through here, so the split above cannot leak into a caller that adds only one
       of them. `base` is where the account entered the order and `earned` is what it has emitted since.
       AND THE FIRST OF THE TWO IS A RELATION WHILE THE ACCOUNT HAS NEVER BEEN SERVED, which is the whole of
       what `placed` says. An unplaced account has emitted nothing (only the flow holding the thread can be
       credited, and holding the thread is what places an account), so its reward IS the frontier's virtual
       time — re-read here at every pick rather than copied once at its creation. That is start-time fair
       queueing's `max(v(t), F)` as a CONTINUING relation: a member the order has never reached does not fall
       behind the clock, and the instant it is served its tag freezes and it stands on its own ledger like
       every other account.
       THE `+ earned` IS WRITTEN ON BOTH ARMS RATHER THAN FACTORED OUT, and the assert says why: it is zero on
       the unplaced arm by construction, so factoring it would read as an arm where a ledger is DISCARDED
       rather than one where it is empty, and the day an emission could reach an unplaced account the two
       spellings would differ by exactly the findings nobody could see going missing. */
    if (!f->family) return 0.0;                                     /* departed: its node is gone, so is its rank */
    DCHECK(f->family->placed || f->family->earned == 0.0,
           "an account that has never held the thread has been credited with an emission — only the running "
           "flow is credited and a dispatch is what places an account, so either a detector credited a flow "
           "that is not in service or an account was placed and then un-placed, and this account's reward is "
           "now the frontier's clock plus a ledger the clock knows nothing about");
    return (f->family->placed ? f->family->base : frontier_vt()) + f->family->earned;
}

double flow_reward(const Flow *f) {
    DCHECK(f != NULL, "the WFQ's reward term was asked of no flow — a reward belongs to a fork family and a "
                      "flow is how one is reached, so there is no family here for this to be the reward of");
    return acct_family_val(f);
}

/* A REBUILT FLOW'S ACCOUNT, REPLACED RATHER THAN INHERITED — the ONE writer of the reward that is neither an
   emission nor an arrival, and the only caller is the cold tier's rebuild.
   WHY IT IS A REPLACEMENT AND NOT AN ADD. flow_add places a from-baseline flow at the frontier's virtual time,
   which includes the reward tag, because a newcomer with no account of its own must enter beside the flow in
   service rather than below the whole frontier. A rebuilt flow is not a newcomer: it is a member coming BACK,
   carrying the account the session that parked it wrote down. Adding would credit this session's findings to
   last session's flow; leaving the placement would silently discard the cross-session ordering the park exists
   to preserve, which is §Time-travel's "a high-value flow suspended last week resumes ahead of a low-value
   fresh one today".
   IT MAY ONLY EVER WRITE A FAMILY OF ITS OWN. A rebuilt flow founds its own family by construction (it is
   from-baseline, so flow_add_unseeded minted it a root and never joined it to anything), and writing through a
   JOINED tag would silently re-place every arm of somebody else's family — the one way this entry could reach
   past the flow it was handed. Asserted here rather than trusted, because the caller cannot see the tag. */
void flow_restore_reward(Flow *f, double val) {
    DCHECK(f != NULL && f->acct != NULL && f->family == f->acct && f->acct->up == NULL,
           "a rebuilt flow's parked reward was written through a fork family it does not own — a resume founds "
           "its own account, so a joined tag here re-places every live arm of another family at a reward that "
           "belongs to one parked recipe");
    DCHECK(val == val && val >= 0.0 && val < 1e300,
           "a parked reward came back as something the WFQ cannot order by — a NaN compares false in both "
           "directions, so the resumed frontier's order would fall back on registry position");
    /* A RESUME RESTORES A COORDINATE, NOT EARNINGS, and the split is what lets that be said. The parked
       number is where this recipe STOOD when the session that wrote it ended; crediting it as `earned` would
       report last session's findings as this session's on the `self_emit` row and would make the census's
       forgiveness identity claim this account had emitted without ever bumping its generation. `base + earned`
       is unchanged, so §Time-travel's "a high-value flow suspended last week resumes ahead of a low-value
       fresh one today" holds exactly as before.
       AND IT IS A TAG AND NOT A READING, WHICH IS THE ONE LINE BELOW IT. `base` is only read at all once the
       account is PLACED (FlowAcct's `placed`), so writing the parked number without placing the account would
       store last session's coordinate and then rank the recipe at THIS session's clock, discarding exactly the
       cross-session ordering the park exists to preserve. A rebuilt flow is not a newcomer — it is a member
       coming BACK with a tag of its own — so it is placed here, by the same statement that writes the tag.
       NAMED RESIDUAL — A RECIPE PARKED BEFORE IT WAS EVER SERVED COMES BACK PLACED, AND IT SHOULD COME BACK
       READING. Not covered: an account whose flows never once held the thread is parked as a single number
       (cold.c writes `%.17g` of flow_reward, which for such an account is the coordinate THIS session's clock
       stood at) and is restored here as a frozen tag — so a never-served recipe re-enters the next session at
       a foreign clock's reading rather than continuing to track the one it is now a member of, which is the
       one-time copy the arrival rule stopped making, arriving through the tier instead of through the door.
       It is CORRECT for the population the entry was written for — a member coming back that HAS earned — and
       narrower than the rule, which is why it is named here rather than crashed on: the value is a real
       coordinate and there is nothing wrong to abort at. What the next diff builds: a park record that carries
       the account's two quantities apart, as `base` and `earned` are apart here, plus the placement bit, so a
       rebuild restores a served account at its own tag and a never-served one UNPLACED; that record's format
       and its writer are cold.c's, and this entry's signature is one number today. How its absence shows: a
       resumed session reporting `valUnplaced: 0` at its FIRST census while rebuilt recipes are standing, with
       `valMin` pinned at a coordinate no member of this session has ever been served at — the same signature
       the arrival copy left, one session removed. */
    f->family->base   = val;
    f->family->placed = 1;
    /* …AND THE CLOCK LEARNS ABOUT IT, WHICH IS THE THIRD AND LAST WRITER OF A REWARD. §Time-travel has "a
       high-value flow suspended last week resumes ahead of a low-value fresh one today", and a coordinate that
       leads the frontier is precisely one the frontier's clock has to know: `g_vt` is what every UNPLACED
       account and every newcomer is ranked at, so a restored leader the clock did not learn about leaves
       arrivals entering BELOW a member they are entitled to tie — for the rest of the session, with `valMin`
       pinned at a coordinate the frontier has left behind and no row saying so. That is the arrival-copy
       defect one session removed, and it is the SAME signature the residual above describes for the other
       half of this entry.
       IT IS ALSO WHAT MAKES frontier_vt_serve's INCREMENTAL MAXIMUM EXACT rather than approximate: that
       function's claim is that every increase of any account's `acct_family_val` passes through it, and this
       line is one of the three sites the claim enumerates. Without it the assertion there fires, which is the
       correct outcome and the reason the two landed together. The serve also raises the frontier generation,
       which a rebuild owes for its own sake: a member coming back at a tag of its own has changed the order. */
    frontier_vt_serve(f);
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
    /* …AND THE SAME EVENT COUNTED FOR THE LIFE OF THE INSTANCE, which `g_gen` cannot answer however much it
       looks like it could. flow_registry_init RESETS `g_gen` and does not reset the scan counters, so the
       generation NUMBER and `scanRivalRuns` would be two quantities over two lifetimes the moment a host
       initialised a second registry — the reader would divide one by the other and get a ratio about no run at
       all. That is the lifetime-over-instant collapse this file already corrected once in build.mjs, and the
       cure is the same: make the two commensurable AT THE SOURCE rather than assume the call site that would
       break them is never reached. Counted here because this is the one place a rank change happens. */
    g_rank_changes++;
    JS_RequestFlowYield();
}

/* THE FRONTIER'S MEMBERS, in registry order. A walk, not a rank: the pending-fetch register lives on the flows,
   so whoever asks what the host owes has to visit all of them. flow_best answers a different question. */
Flow *flow_at(int i) { return (i >= 0 && i < g_flows_n) ? g_flows[i] : NULL; }

void flow_registry_init(const char *doc_name) {
    g_flows = NULL; g_flows_n = 0; g_flows_cap = 0; g_gen = 0; g_running = NULL;
    /* THE QUEUE'S CLOCK COMES UP WITH THE QUEUE, at SFQ's v(0) = 0. Left standing from a previous registry it
       would place this frontier's first from-baseline flows at the coordinate a DIFFERENT document's leader
       had reached — a lifetime quantity read as an instant, which is the collapse `g_rank_changes` beside
       `g_gen` already records one instance of. */
    g_vt = 0.0;
    /* THE WORLD NAMESPACE COMES UP WITH THE FRONTIER, not beside it: every flow added below is minted a world
       named by this document, and a frontier whose worlds were unnamed could not be reached from another
       instance at all. */
    world_registry_init(doc_name);
}

unsigned flow_frontier_gen(void) { return g_gen; }

/* HOW MANY TIMES THE ORDER CHANGED, for the life of this instance — the denominator the preempt hook's
   rescan count has and `scanNextRuns` is not. See solver/flow.h. */
long flow_rank_changes(void) { return g_rank_changes; }

/* HOW MANY TIMES THE SCHEDULER'S OWN PICK RETURNED AN ALREADY-DISPATCHED MEMBER WHILE A NEVER-DISPATCHED ONE
   STOOD AT EXACTLY THE SAME WEIGHT — §scheduler's razor's STARVES, counted at the ONE line that decides which
   flow runs, and the reading that three census GAUGES could not make between them. See solver/flow.h. */
static long g_starved_picks = 0;
long flow_starved_picks(void) { return g_starved_picks; }

/* A FLOW TAKES OR RELEASES THE THREAD — and a dispatch is where an account's coordinate STOPS being a reading
   of the frontier's clock and becomes a tag of its own. The two statements are ONE operation and in this order,
   because between them the clock would be defined by an account that is defined by the clock.
   THE STAMP IS VALUE-PRESERVING BY CONSTRUCTION AND IS ASSERTED AS SUCH. An unplaced account's reward IS
   frontier_vt(), so freezing that same reading into `base` cannot move the flow's queue coordinate by so much
   as an ulp — a dispatch is not a re-ranking. What the assertion catches is a stamp that writes some OTHER
   quantity (the flow's own weight, the incumbent's reward, a sum that has picked up a per-member reading), any
   of which would move a member at the instant it is picked and make the pick that led there a decision about a
   weight the member no longer has.
   RELEASING THE THREAD LEAVES THE CLOCK WHERE IT IS, which is the idle case and is deliberate: v(t) holds
   between busy periods, so a newcomer created while the host is between two slices enters at the coordinate
   the frontier last stood at rather than at zero. */
void  flow_set_running(Flow *f) {
    if (f && f->family && !f->family->placed) {
        double w = flow_queue_weight(f);
        DCHECK(f->family->earned == 0.0,
               "an account is being placed with a ledger already on it — placement freezes the frontier's "
               "clock into `base`, and a non-zero `earned` here means this account emitted before it ever held "
               "the thread, so the reward it is about to be ranked on counts one finding twice");
        f->family->base   = frontier_vt();
        f->family->placed = 1;
        DCHECK(flow_queue_weight(f) == w,
               "freezing the frontier's clock into an account's tag moved the flow's queue coordinate — an "
               "unplaced account's reward IS that clock, so the stamp must be exact; a dispatch has just "
               "re-ranked the member the WFQ picked, and the comparison that picked it was about a weight this "
               "flow no longer carries");
    }
    g_running = f;
    /* AND THE CLOCK IS **NOT** MOVED HERE, WHICH IS THE ONE SUBSTANTIVE THING THIS FUNCTION NO LONGER DOES.
       It used to serve the clock at this line — "the clock now reads the item in service" — and that made a
       DISPATCH re-rank every account still reading the clock, AFTER the pick had compared them and BEFORE
       engine.c records what the winner was ranked on. The arithmetic is exact and is why this is a removal
       rather than a tuning: serving here sets the clock to the winner's own queue coordinate, so an unplaced
       never-run account R immediately stands at `g_vt + 1.0 + distance(R)` while the flow just dispatched
       stands at `g_vt + 1/(1+visits) + distance` — R outranks it STRICTLY the moment it has completed one
       unit of work, for every dispatch, by construction, with no term of the winner having moved at all. The
       pick's own choice promoted the tail above it, and the value-yield assertion in engine.c fired on a
       state whose five clauses are all terms of the winner and therefore cannot see it.
       AND THE TREE'S ARGUMENT FOR FOLLOWING THE ITEM IN SERVICE IS ABOUT THE EMISSION, NOT ABOUT THIS. See
       frontier_vt_serve's remaining caller: "without it, every account that has never been served would stand
       at the coordinate the leader held when it was last picked, and would be passed by exactly the findings
       the leader makes during the slice it is holding". That sentence is about FINDINGS, and it is answered
       in full by the emission caller alone. FlowAcct's `placed` rejects a coordinate "taken ONCE, at
       creation" for the same reason and is likewise silent about dispatches. Nothing in this file ever argued
       that a newcomer's coordinate should follow WHICH FLOW HOLDS THE THREAD.
       WHAT THIS LINE'S REMOVAL BUYS IS THE PROPERTY THE HOOK NEEDS: every writer of the clock now raises the
       frontier generation (frontier_vt_serve), so the pick and the preempt hook read one clock and a rival
       cached at a generation is a rival the pick was shown. A yield after an emission is then a legitimate
       rank change through clause one, which is what engine.c's assertion has been asking for.
       PLACEMENT ABOVE IS STILL RANK-NEUTRAL AND RAISES NOTHING: freezing `base` stops this account reading
       the clock and starts it standing on the identical value, which the DCHECK above asserts exactly. */
}
Flow *flow_running(void) { return g_running; }

/* Credit the running flow with newly EMITTED value (a new @H endpoint / @S PoC): its WFQ reward rises and its
   CPU-since-emit aging resets, so a productive flow outranks fresh + starved flows. Called by the detectors
   (endpoint.c / solve.c) when they record something NEW — this is what makes the WFQ value-of-information
   ordered rather than merely breadth-first by visit count. The frontier gen bumps so the value-yield re-ranks. */
void flow_credit_emit(double v) {
    int64_t own_us;   /* the emitter's own burn over the window this emission closes — see the bump below */
    /* ONE EMISSION IS ONE POINT, and the aging term's exchange rate is stated against exactly that. A credit of
       zero or less would leave a flow's rank unchanged while resetting its aging, which is the one way a
       monopolizer could refresh its lead without producing anything — so the reward's sign is asserted where it
       enters rather than assumed by the comparator that subtracts against it. */
    DCHECK(v > 0.0, "a flow was credited a non-positive emission — the WFQ's reward term counts NEW @H/@S "
                    "findings at one point each, and a zero credit resets the aging that outranks a monopolizer "
                    "while adding nothing to weigh it against");
    if (!g_running) return;
    /* THE ORDER'S REWARD IS THE FAMILY'S, so the credit lands on the family — see FlowAcct's `val`. This is the
       whole of the difference between a credit that can be paid off and one that cannot: raise the family once
       and the silence charged to that same family cancels exactly it; raise every ARM instead and one emission
       is presented as many times as the page's bundle happens to branch, against a debit charged once.
       IT MAY NOT BE ROUTED THROUGH A DEPARTED NODE. A running flow is a member of the frontier and a member
       owns its account, so a NULL family here is a flow that was credited after it left — the emission would
       be recorded nowhere and the finding would rank nothing. */
    DCHECK(g_running->family != NULL,
           "the flow holding the thread belongs to no fork family, so there is nothing for this emission to be "
           "credited to — a departed flow was left running, and the reward this finding is worth is about to "
           "be dropped on the floor while the detector reports it as recorded");
    DCHECK(g_running->family->placed,
           "the flow holding the thread belongs to an account that has never been dispatched — a dispatch is "
           "what places an account and this credit is being made from inside one, so the two writers of that "
           "fact have come apart and this account's reward is about to be a ledger stacked on a clock reading "
           "that is still moving underneath it");
    g_running->family->earned += v;
    /* …AND THE FRONTIER'S CLOCK, BECAUSE THE ITEM IN SERVICE IS WHAT THE CLOCK IS A READING OF AND ITS TAG HAS
       JUST MOVED. This is frontier_vt_serve's ONLY event now, and it is the one that makes v(t) a CONTINUING
       relation rather than a coordinate copied once: without it, every account that has never been served
       would stand at the coordinate the leader held when it was last picked, and would be passed by exactly
       the findings the leader makes during the slice it is holding — which is the arrival copy going stale
       again, one slice at a time instead of once. The clause that used to say "second of two" named a DISPATCH
       as the other, and that write is gone; this sentence is the whole of the argument for the one that
       remains, which is why it is stated in terms of FINDINGS and not of who holds the thread. */
    frontier_vt_serve(g_running);
    /* …AND THIS ONE MEMBER'S OWN LEDGER, WHICH RANKS NOTHING AND IS NOT A SECOND COPY OF THE LINE ABOVE. The
       family total says what an ACCOUNT has emitted and is the order; this says what THIS FLOW emitted, which
       is the one question the total structurally cannot answer and the one the census exists to ask — a
       frontier retiring members steadily while every one of them reads zero here is a frontier coasting on an
       ancestor's findings. It is never copied, never inherited and never read by flow_weight, which is what
       keeps it a measurement rather than a rank. */
    g_running->val += v;
    /* …AND **NOT** THE OPTIMISM TERM'S QUANTITY, WHICH THIS USED TO ZERO ON THE EMITTER. `g_running->visits = 0`
       stood here, and the line above it is what retires the argument that justified it: that argument said a
       flow which has just produced something "then leads by its REWARD", and the reward is the FAMILY's, so
       within a family it is a common offset that leads NOBODY — the emitter and every arm of it read the same
       `val` through the same pointer. Its second clause ("the only thing keeping it in front is `val`") is the
       same sentence and is stale for the same reason, and its third described the ordinary rotation as a
       failure: an arm inherits the emitter's count at the branch, so the emitter falls behind it by exactly ONE
       completed unit, which is fair queueing rather than a flow being buried.
       IT WAS RESIDUE OF THE TERM THE COUNT REPLACED. While the optimism term read `cpu / FLOW_SERVICE_US`,
       forgiving the silence and forgetting the trials were ONE statement in ONE field, and zeroing on an
       emission was coherent. When the quantity became a COUNT OF COMPLETED UNITS those stopped being one
       statement; only the silence half was re-derived (FlowAcct's `emit_gen`, below), and the count's zero was
       carried across the re-keying verbatim.
       WHY IT WAS WRONG AND NOT MERELY STALE — in this file's own words at FlowAcct's `val`, about this exact
       pair: two arms of one parent, forked at two instants, "differ by exactly what the parent emitted in
       between — which neither of them did". A zero written here is COPIED by every fork the emitter takes
       afterwards, so an arm born after an emission carried a full 1.0 while its sibling born before it carried
       `1/(1+V)` — the optimism term's whole range apart, on an event BOTH of them are already credited for
       once through the account they share. One credit minted once was presented twice: to the family through
       `val`, and again to whichever member happened to be holding the thread.
       AND THE FIELD THAT WOULD HOLD SUCH A PREFERENCE ALREADY EXISTS AND IS DELIBERATELY UNRANKED. `Flow.val`
       one line up is what THIS member emitted, and it is never read by flow_weight — which is what keeps it a
       measurement rather than a rank. A within-family preference for the productive member is therefore a
       thing this accounting has already refused, in the field built to hold it, and the zero was that refusal
       being overturned through the optimism term instead.
       WHAT ORDERS A FAMILY'S MEMBERS INSTEAD is what §scheduler asks of it: fewest completed units first and
       least own silence first — fair queueing over the arms, with the value-of-information ordering carried
       BETWEEN families by the reward. A flow that emits keeps the thread until it finishes the unit it is
       inside (flow_pick's comparison is STRICT and its arms tie with it), its count then advances, and it
       hands over on its own visit.
       SO AN EMISSION DOES NOT MOVE THIS TERM AT ALL, and `visits` is now one quantity with one meaning: the
       units of work completed on this flow's own prefix, raised only by flow_credit_visit and carried by a
       fork. The rank still changes on an emission — through the reward, both silence halves and the generation
       bump below — so nothing that watches for one loses its signal. */
    /* …AND THE AGING, BOTH HALVES OF IT, IN ONE STATEMENT ABOUT THE ACCOUNT. This used to be two: `fam_us = 0`
       here and `g_running->cpu = 0` above it, and the pair was described as "deliberately no ordering between
       the resets — a sibling's emission forgives the family while this flow's own `cpu` stands, and that is the
       policy rather than a drift". THAT ARGUMENT IS RETIRED, and FlowAcct's `emit_gen` carries the refutation:
       the two halves are SUMMED into one notch and weighed against ONE reward, and that reward is the FAMILY's,
       so a debt measured over a window the family's income does not cover is a unit error rather than a policy.
       Its consequence was a population, not a rounding difference: `Flow.cpu` is written only for the flow
       holding the thread, so a member the scheduler has never dispatched could never reach either reset, and it
       carried its parent's fork-instant burn as a permanent deficit against every arm that did emit.
       ONE EMISSION, ONE ACCOUNT, ONE FORGIVENESS. The generation bump is what makes the own half readable as
       zero for every member of this family at once, in O(1) and with no walk of a frontier that reaches
       thousands; `fam_us` is the family half of the same window and is zeroed beside it. Neither is a second
       policy — they are the two scopes flow_silence_notch already sums, now sharing the epoch it always claimed
       they shared.
       WHAT IT DOES NOT DO IS FLATTEN THE ORDER. Every member reads zero own-silence only until the thread is
       handed to one of them: the flow that then holds it is charged (flow_age_running) while its siblings are
       not, so it sinks at FLOW_AGE_QUANTUM per quantum against every arm of its own family — which is exactly
       the within-family comparison the own half exists to make, and now it is a comparison the losing side can
       win back by waiting rather than a debt only a dispatch could clear. */
    /* WHAT THE EMITTER ITSELF HAS BURNED OVER THE WINDOW THAT IS ABOUT TO CLOSE — read THROUGH
       flow_own_silence and BEFORE the bump, because the raw `Flow.cpu` is that quantity only while its mark is
       current: a flow that emits twice with no charge in between holds a previous window's arithmetic in the
       field, and every reader is already answering ZERO for it. Reading the field would resurrect it. */
    own_us = flow_own_silence(g_running);
    g_running->family->emit_gen++;
    /* …AND THE EMITTER CARRIES ITS OWN BURN ACROSS THE BUMP, WHICH IS THE ONE THING THIS FORGIVENESS MAY NOT
       DO FOR THE MEMBER THAT EARNED IT. The bump forgives the own half for every arm of the family at once,
       and that is right for every arm EXCEPT the one holding the thread: CLAUDE.md's rule is that the credit
       and the debit live at the same accounting unit or neither term means anything, and here the credit is
       the FAMILY's (`earned`, read by every arm through one pointer) while the debit erased is the MEMBER's
       (`cpu`, charged to one flow). One credit, two beneficiaries — the family through the ledger, and again,
       privately, whichever member happened to be holding the thread.
       THIS FUNCTION ALREADY MADE THAT EXACT ARGUMENT ONE TERM OVER AND DID NOT CARRY IT HERE. `g_running->
       visits = 0` stood a few lines above and was deleted for it, in these words: "One credit minted once was
       presented twice: to the family through `val`, and again to whichever member happened to be holding the
       thread." The optimism term's zero went; the silence term's did not, and the silence term is the ONLY
       within-family ordering signal left once the reward and the family half have cancelled as the common
       offsets they are.
       WHAT IT COST, DERIVED RATHER THAN GUESSED. flow.h's `never_picked` block names the two writers that end
       an incumbent's hold: flow_credit_visit, which asserts `frame == NULL` and so cannot fire for a member
       inside a program (a fork copies `visits`, so a chain of framed arms reads one bonus for all of them),
       and this charge. With both inoperative, every member of a one-family frontier reads ONE weight, the
       pick's STRICT comparison leaves the thread where it is, and the never-dispatched tail stands at exactly
       the front for ever. MEASURED, at two revisions and on seven runs: `neverPickedAtTop == neverPicked`
       EXACTLY at every zero-gap sample, at 46, 259, 269, 285 and 570 members.
       SO THE WINDOW CLOSES FOR THE FAMILY AND NOT FOR THE ARM THAT SPENT IT. `fam_us` becomes what its members
       still owe, which after the bump is the emitter's burn and nothing else, so the relation the aging's two
       halves owe each other holds with EQUALITY at this transition rather than at zero.
       NAMED RESIDUAL. Not covered: an arm that burned the thread earlier in this window and did NOT emit still
       has its own half forgiven here, because the bump reaches it and nothing holds it in hand. That is
       narrower than fair queueing over the arms, which would remember every arm's service and not just the
       emitter's; it is CORRECT for what it does — the emitter is by construction the arm that must hand over
       next, so this is the one retention that changes which flow runs — and it is a one-emission memory rather
       than a ledger. What the next diff builds, if the sweep still stalls: the own half kept per arm across
       the bump, which needs a per-member window mark that a bump cannot reach in O(1) and is therefore a
       change to how `emit_gen` forgives, not another line here. How its absence shows: `picksMax` staying at
       the mean sweep depth between two emissions (flow.h's `top_forgiven` reading) while `neverPicked` climbs
       with `members` — a sweep that restarts rather than one that stalls.
       AND IT NARROWS flow_pick's `unrun` POPULATION, WHICH A GUARD THAT FIRES LESS MUST SAY AS PLAINLY AS ONE
       THAT FIRES MORE. That set needs every non-reward term at zero, and `fam_us` is now non-zero immediately
       after an emission rather than zero — so within one quantum of a finding the two ordering guards there
       short-circuit where they used to be live, but ONLY when the emitter burned a whole cooperative quantum
       between two findings, since a `fam_us` below FLOW_SERVICE_US still floors to a notch of zero. */
    g_running->cpu             = own_us;
    g_running->cpu_gen         = g_running->family->emit_gen;
    g_running->family->fam_us  = own_us;
    /* THE TWO HALVES ARE EQUAL FOR THE EMITTER AND ZERO FOR EVERY OTHER ARM, which is the relation stated at
       its strongest: the family's burn over the new window IS the emitter's, because the emitter is the only
       arm whose burn survived the bump. The transition the old code broke was the mirror of this one — the
       family half zeroed while a sibling's own half held a previous window's burn — and it is still caught,
       because the macro is expanded at every transition that can move either half. */
    DCHECK_AGING_ONE_WINDOW(g_running, "an emission forgave this family's window");
    /* THE RANK CHANGE IS RAISED BY frontier_vt_serve ABOVE AND NOT A SECOND TIME HERE, which is a correction
       and not a saving. An emission is ONE event and `g_rank_changes` is a LIFETIME COUNTER of how many times
       the order changed, so two raises for one emission would make every rate a reader builds from it — the
       preempt hook's rescan cadence against it, above all — a ratio over a quantity that counts some events
       twice. The raise moved to the clock's writer because the clock is a term of every unplaced account's
       weight and a write to it must not be able to happen without one; an emission reaches that writer
       unconditionally past the early return, so nothing is lost by not repeating it. The yield request the
       raise carries is a REQUEST bit polled at the next opcode, so raising it before this function finishes
       updating `emit_gen` and `fam_us` is observationally identical: the C statements complete first. */
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
void flow_observe_survival(Flow *f, double frac) {
    DCHECK(f != NULL,
           "an @S candidate's surviving fraction was recorded against no flow — the distance is a fact about "
           "ONE flow's own payload, so a caller with no flow measured somebody else's bytes");
    DCHECK(frac >= 0.0 && frac <= 1.0,
           "an @S candidate's surviving fraction is not a fraction — it is the surviving run over the payload's "
           "own length, so a value outside [0,1] is a run measured against a denominator that is not this "
           "candidate's, and it enters the weight beside an optimism term whose entire range is 1.0");
    DCHECK(f->cand_payload != NULL,
           "a surviving fraction was recorded for a flow carrying no payload — the fraction's denominator IS "
           "the payload, so a flow with none has nothing this number could be a fraction of and would rank "
           "above every exploration flow on a measurement of nothing");
    /* AND THE RUNG BELOW THIS ONE, WHICH IS THE ORDER OF THE LADDER ASSERTED FROM THE LADDER'S OWN SIDE. The
       flow having a payload says the substitution was INSTALLED; it does not say the substitution has HAPPENED,
       and the run this function is about to record is a run of the payload's bytes in a string — which is
       found in the PAGE'S own text just as readily, because a breakout is punctuation. solve.c's three
       candidate-arm entries ask the component that performs the substitution and return before reaching here
       if it has not; this is the same statement one layer down, so a fourth sink class, or a fourth route into
       an existing one, cannot re-open the door by forgetting to ask. It is also what makes flow_distance's
       `cand_surv + cand_rung` composition sound: the fraction is arithmetically BELOW the delivery and
       chronologically above it, and this is the assertion that the pair which would expose that — a nonzero
       fraction standing at rung 0 — cannot exist. */
    DCHECK(f->cand_rung >= FLOW_RUNG_DELIVERED,
           "an @S survival fraction is being recorded for a flow that has not been observed to DELIVER its "
           "payload — the run about to be measured is the page's own bytes coinciding with the candidate's, "
           "so the flow's fitness, the search's ratchet and the report's surviving-byte count would every one "
           "of them be a reading of text the attacker never supplied");
    if (frac <= f->cand_surv) return;  /* not an improvement: the rank did not move, so nothing may re-rank */
    f->cand_surv = frac;
    frontier_rank_changed();
}

/* THE SAME QUANTITY, ONE RUNG UP — see flow.h for why the writers are separate entry points, why the ladder's
 * bottom rung sits below every stage §@S names, and why the fire is not on it. Everything
 * flow_observe_survival says about being a reading rather than a payment holds verbatim here; what this adds
 * is the ORDER, which the fraction does not have.
 * WHY THE ORDER IS ASSERTED RATHER THAN ARRANGED. Each escape site in solve.c runs downstream of the arrival
 * site on the SAME string of the SAME flow, and both run downstream of a DELIVERY because every candidate-arm
 * sink entry returns at its door unless this flow's substitution has been performed — so the predecessor is
 * always already recorded, and the one way that stops being true is the one that matters: the arrival write is
 * refused for a candidate the search's own delivery table has since contradicted, and an escape recorded past
 * a refused arrival would advance a flow onto a rung the search declined to give it. That is not a rank one
 * rung too high; it is a rank identical to a candidate that arrived AND escaped, taken by one the search has
 * measured cannot arrive at all.
 * AND A RE-DELIVERY IS NOT A DEMOTION. A candidate that is RESTARTED rather than resumed (decide_enter, which
 * clears the component's live per-flow state) replays from the baseline and delivers again, so this is called
 * with FLOW_RUNG_DELIVERED for a flow already standing on ARRIVED. The early return below is the whole of the
 * handling: a lower later sample is another look at a question whose answer is fixed — this flow's payload is
 * fixed — never a statement that it has un-delivered. §@S's monotone clause is that sentence exactly. */
void flow_observe_rung(Flow *f, int rung) {
    DCHECK(f != NULL,
           "an @S candidate's rung was recorded against no flow — a rung is a fact about ONE flow's own bytes "
           "standing at ONE sink, so a caller with no flow observed somebody else's");
    DCHECK(rung == FLOW_RUNG_DELIVERED || rung == FLOW_RUNG_ARRIVED || rung == FLOW_RUNG_ESCAPED,
           "an @S rung outside the ladder was recorded — the comparator's denominator is FLOW_RUNGS_N and the "
           "rungs are its numerator, so a value outside them puts the fitness term outside [0,1] and lets a "
           "candidate outrank a flow that has emitted a finding");
    DCHECK(f->cand_payload != NULL,
           "a rung was recorded for a flow carrying no payload — a rung is a statement about where THIS flow's "
           "injected bytes stood, so a flow that injected none has nothing to have stood anywhere and would "
           "rank above every exploration flow on an observation of nothing");
    DCHECK(rung <= f->cand_rung + 1,
           "an @S candidate skipped a rung — its bytes were recorded as having ESCAPED a context they were "
           "never recorded as ARRIVING in, so either the two observations are being made about different "
           "strings, or the arrival was refused for a payload this search has contradicted and the escape is "
           "about to hand that same payload the whole ladder anyway");
    if (rung <= f->cand_rung) return;  /* not an improvement: the rank did not move, so nothing may re-rank */
    f->cand_rung = rung;
    frontier_rank_changed();
}

/* A THIRD KIND OF QUANTITY, AND THE THING TO SAY ABOUT IT IS WHAT IT IS *NOT*. The two above are the WFQ's:
 * one a ledger, one a comparator, both read by flow_weight. This is neither, and it must never become either.
 * It is a fact about the PATH — did this flow ever take an arm the concrete example contradicts — recorded so
 * that a request the flow builds afterwards can declare itself FORCED rather than DERIVED (CLAUDE.md
 * §A-REQUEST-CARRIES-THE-PROVENANCE, and see `path_forced` in flow.h for why no value can answer it).
 * SO IT DOES NOT TOUCH THE FRONTIER GENERATION. §scheduler's fork-is-rank-neutral says a sibling is worth
 * exactly what its parent is worth at the instant of the branch, and BOTH arms of a forced branch are that
 * sibling — the one that agrees with the example and the one that does not. Re-ranking here would price a
 * property of the arm, which is the reset-by-splitting every other term of this file is written to forbid, and
 * flow_fork_inherit's rank-neutrality equality would not catch it because this quantity is not in the weight.
 * IDEMPOTENT BY THE ASSIGNMENT and not by a guard: a path cannot un-take an arm, so the second contradiction
 * says nothing the first did not, and there is no reading to discard. */
void flow_mark_forced_arm(void) {
    /* NO `if (g_running)` GUARD, for flow_age_running's reason exactly. Every caller marks a flow it is
       STANDING IN: decide.c's two decision seams — a bytecode branch, and a native operation's outcome fork
       over a machine's stated real completion — reach this only with a flow switched in (each returns -1
       before asking anything otherwise), and engine.c's two exploration seams (a declined request's failure
       arm, a modelled close request's arrival) each assert `flow_running() == f` at their own site before
       calling. So a mark with nothing running is a fact recorded on a path belonging to no flow — and whatever
       took it would then go on to build requests that declare themselves DERIVED.
       THIS LIST IS A CLAIM ABOUT THE TREE AND IS KEPT WHERE THE FIELD IS DESCRIBED. The sentence that stood
       here named decide.c as the whole of the callers and had been false since the decline fork landed; the
       standing list is flow.h's, beside `path_forced`, and this is the local restatement of why the guard is
       absent rather than a second copy of it. */
    DCHECK(g_running != NULL,
           "an arm contradicting its own example was recorded with no flow running — the contradiction is a "
           "fact about ONE path, so there is no flow here to be standing on it and the requests built past "
           "that arm would declare themselves DERIVED");
    g_running->path_forced = 1;
}

int flow_path_forced(const Flow *f) {
    DCHECK(f != NULL, "a flow's path was asked whether it stood on a forced arm, of no flow");
    DCHECK(f->path_forced == 0 || f->path_forced == 1,
           "a flow's forced-path mark is neither set nor clear — it is written in exactly one place, as a "
           "constant, so any other value is memory this field does not own");
    return f->path_forced;
}

/* Age the running flow by the MICROSECONDS of thread time its step just burned. A monopolizer that runs without
   emitting sinks below productive + unrun flows — see FLOW_AGE_RATE for the exchange that makes that true.
   IT IS THE ONLY CHARGE ON `cpu`, AND THAT SENTENCE IS A CORRECTION. What stood here said a DEPARTING flow
   hands what it burned to the flow that forked it — a mechanism acct_depart's own comment records as DELETED,
   because `fam_us` counts every microsecond the family burns AS IT BURNS IT and a hand-up at departure would
   bill it twice. The monopolizer this term is priced against is still the fork CHAIN rather than one of its N
   names; what charges the chain is the family half below, not a residual moved at a departure.
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
    /* THE ACCOUNT BOTH HALVES OF THIS CHARGE LAND ON, ASSERTED BEFORE EITHER OF THEM READS IT. It stood below
       the `cpu` charge while that charge touched only the flow; the own half is now read over the ACCOUNT's
       silence window (FlowAcct's `emit_gen`), so the family pointer is dereferenced by the very first
       statement and the assertion has to precede it or it is checking a pointer already used. */
    DCHECK(g_running->family != NULL && g_running->family->up == NULL,
           "the running flow's family tag is not the ROOT of a fork tree — an arm whose tag points at its own "
           "node has FOUNDED a family instead of joining its parent's, which is precisely the reset by "
           "splitting the whole of this accounting exists to forbid: it would carry its parent's reward with "
           "none of its parent's aging and outrank the entire backlog for free");
    /* THE WINDOW THIS CHARGE BELONGS TO, NORMALISED BEFORE IT IS ADDED TO. `Flow.cpu` is a reading only while
       `cpu_gen` matches the account's generation (flow_own_silence); past an emission the field holds a
       previous window's arithmetic that every reader is already answering ZERO for, so adding to it here would
       resurrect a quantity nothing was reading and charge this flow for thread time it burned before its
       family's last finding. Writing the field to what its readers already return is observationally a no-op —
       which is what makes it safe at a site the preempt hook's seam assertion straddles — and it is done HERE
       because this is a write site already; flow_own_silence stays read-only for the reason it states.
       NO `if` PAST A BROKEN INVARIANT: a stale generation is the ORDINARY state of every member of a family
       that has just emitted, so this is the normal path and not a repair. */
    if (g_running->cpu_gen != g_running->family->emit_gen) {
        g_running->cpu = 0;
        g_running->cpu_gen = g_running->family->emit_gen;
    }
    g_running->cpu += us;
    /* …AND THE SAME MICROSECONDS ON THE FAMILY. Both are the AGING term's — it is their SUM — and they are not
       two copies of one number, because they answer two different comparisons over ONE window: `cpu` is what
       THIS FLOW has burned since the family last emitted, while `fam_us` is what the WHOLE family has burned
       over that same window. The first is what orders a family's members against each other and the second is
       what orders one family against another, and a term with only the second has nothing to say on a frontier
       that is a single family — which a real page's is.
       ONE WINDOW AND NOT TWO, WHICH IS THE CORRECTION THIS BLOCK CARRIES. The own half used to be measured
       since this flow's OWN last emission — a strictly longer window than the family's, since a flow's own
       emission is one of its family's — so the two were summed into one notch and weighed against a reward
       credited over the shorter one. See FlowAcct's `emit_gen` for what that cost and for the population it
       could never reach: a member the scheduler has never dispatched is never charged here and so could never
       be forgiven here either.
       Neither of them is the OPTIMISM term's quantity; that one is a count of completed units of work and is
       charged by flow_credit_visit, which is why it is a separate call from this one. */
    g_running->family->fam_us += us;
    /* THE SAME MICROSECONDS WENT TO BOTH HALVES, WHICH IS WHAT KEEPS THEM ONE READING AT TWO SCOPES. This is
       the only site that ADDS to either, so it is the only one that could add to one and not the other — a
       charge that reached `cpu` and missed `fam_us` would make an arm's own silence outgrow its family's and
       put the sum back in two epochs. */
    DCHECK_AGING_ONE_WINDOW(g_running, "a slice of the thread was charged to this flow and its family");
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

/* THE SCHEDULER CHOSE THIS MEMBER — see flow.h's `picks` for why a DISPATCH count is the only statement about
 * a member that an emission cannot erase, and flow.h's `never_picked` for the reading it makes possible.
 * IT DECIDES NOTHING AND IT MOVES NO RANK. flow_weight does not read it, no fork carries it and nothing resets
 * it, so this entry cannot reorder the frontier it is counting — which is the whole property that makes it a
 * measurement rather than a second clock. It is a `++` with an assert in front of it on purpose: the moment
 * this quantity acquires a consumer inside the ordering it stops being able to answer the question it exists
 * for, and flow_fork_inherit's rank-neutrality equality is what would fire if it ever did. */
void flow_credit_pick(Flow *f) {
    DCHECK(f != NULL, "the scheduler credited a dispatch to no flow — this is the one point every context "
                      "switch converges on, so a NULL here is a switch that ran with nothing switched in");
    DCHECK(flow_is_member(f),
           "the scheduler credited a dispatch to a flow that is not a member of the frontier — the census row "
           "this feeds is taken over the members, so a departed flow's dispatch would be counted nowhere and "
           "the starvation reading would be a fraction of the wrong population");
    f->picks++;
    /* …AND THE SAME EVENT COUNTED FOR THE LIFE OF THE INSTANCE, which the per-member field cannot answer for a
       reason that has nothing to do with resets: a member that DEPARTS takes its share of this total off the
       frontier, so the sum over the members standing now is a gauge and the difference between two of those
       gauges is not a number of dispatches. Counted here because this is the one line every dispatch passes
       through, so the two cannot be credited at different moments. */
    g_picks_total++;
}

/* IS THIS FLOW'S JAVASCRIPT EXECUTION CONTEXT STACK EMPTY?
   HTML §8.1.4.4 "Calling scripts", clean up after running script step 3: "If the JavaScript execution context
   stack is now empty, perform a microtask checkpoint." That sentence is the precondition of a MICROTASK
   checkpoint and of a TASK alike — §8.1.7.3 "Processing model" runs one task and then performs a checkpoint,
   so neither may begin part-way through a program — and it has two halves here, only one of which is a field.
   `Flow::frame` IS that stack (`the current script's live preemptible frame, NULL between scripts`,
   solver/flow.h), so a live frame answers no outright.
   THE OTHER HALF IS THE ROW AT THE CURSOR. §4.12.1.1 "Processing model" ends "prepare the script element" with
   "Otherwise, immediately execute the script element el, even if other scripts are already executing" — that
   program runs INSIDE the one that inserted it, so the stack has NOT emptied across it and nothing the event
   loop would otherwise pick may run in front of it. Every OTHER program the flow has left is a task: the
   document's next <script>, a lazy chunk, a `javascript:` URL, a §8.7 Timers string handler, a peer's
   operation.
   IT IS ONE PREDICATE AND NOT A CONDITION EACH ARM RESTATES, which is the whole reason it is a function: the
   microtask checkpoint below and the networking task source's arm in flow_step are two consumers of ONE spec
   sentence, and a second spelling of it is a second copy that disagrees eventually. A caller that has already
   established `!f->frame` pays one field read for the half it knows. */
int flow_stack_empty(const Flow *f) {
    int row;

    if (f->frame) return 0;
    row = f->script_i;
    if (row < f->dyn_n) {
        DCHECK(f->dyn_pos != NULL,
               "a flow holds queued programs with no position column — the row the cursor names cannot say "
               "whether it is a task or the synchronous tail of the program that queued it, and the microtask "
               "checkpoint is placed against exactly that");
        if (f->dyn_pos[row] == DYN_POS_IMMEDIATE) return 0;
    }
    return 1;
}

/* AND ITS SECOND CONSUMER IS THE CENSUS, WHICH IS WHY IT LIVES HERE AND NOT IN THE SCHEDULER'S FILE. The
   engine's reply-delivery arm is guarded on this predicate, so the number of members it is TRUE of is the size
   of the population that can take a reply at all — and `framed` is not that number: it answers only the first
   of the two halves below, so `flows - framed` is an upper bound and never the count. cold_census asks this
   directly (solver/cold.h's `stack_empty`), which is what lets a run distinguish "the frontier is drowning in
   replies its members cannot take" from "the arm is not being reached for some other reason". Those are two
   readings of one zero and they take opposite work. */

/* A FORKED SIBLING IS A CONTINUATION, NOT A NEWCOMER — so it inherits EVERY term of its parent's account, and
 * this is the one place that says so.
 *
 * A fork is "append an arm to a decision vector" (flow.h): the sibling holds the parent's frame snapshot, the
 * parent's COW delta, the parent's DOM base, the parent's queued jobs and the parent's outstanding replies —
 * every field of the parent's HISTORY is copied at the fork except the ones the WFQ ranks by, which were left at
 * the constructor's zeros. Each zero is a false statement about that flow, and each is false in the direction
 * that promotes it:
 *
 *   the REWARD — the sibling has, by construction, executed every emission the parent made before the branch;
 *   they are in its own delta and its own recipe replays them. Zeroing it says the flow that found six
 *   endpoints and then branched produced nothing, and ranks its two continuations below arms nobody has looked
 *   at.
 *   AND IT IS NO LONGER COPIED AT ALL, WHICH IS THE STRONGER FORM OF THE SAME RULE AND NOT A RETRACTION OF IT.
 *   A copy is neutral between a parent and the arm born AT THAT INSTANT, and this function's own equality is
 *   written over exactly that pair — so it could never see the pair it does not compare: two arms of one
 *   parent, forked at two instants, which must ALSO be worth the same, because neither of them did anything
 *   between the two branches. A copied reward makes them differ by what the PARENT emitted in between, which
 *   is birth order and nothing else, and an order read descending on birth order is newest-first. The reward
 *   lives on the fork family (FlowAcct's `val`) and the arm JOINS that family below, so there is nothing to
 *   copy and nothing a fork can carry differently — at this instant or at any later one.
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
 * "NEVER STARVED IS TRUE AMONG MEMBERS OF EQUAL REWARD AND AMONG NO OTHERS" IS STILL THE ARITHMETIC, AND THE
 * PARAGRAPH THAT DERIVED IT WENT ON TO OFFER A JUSTIFICATION THE FRONTIER FALSIFIED. It said that with `val`
 * monotone down every fork chain, "the entire tree below that prefix sits above 1.0 and a from-baseline flow
 * is reached when that tree DRAINS", and called that a real ordering with a real justification. The tree does
 * not drain. Measured on the smoke fixture, three runs, one of them uninterrupted to the census that stopped
 * changing: the frontier grew roughly twenty-four fold while the top of the reward ledger never advanced once
 * and the floor never moved off its second reading, with the whole spread — a hundred and sixty-eight points
 * against every other term's range of one — standing between two populations neither of which had emitted
 * anything ITSELF. A justification that rests on a drain, on a frontier that grows, is not a weaker guarantee
 * than the one it replaced; it is the same starvation with a reason attached.
 *
 * WHAT IS ACTUALLY WRONG IS NOT THE INHERITANCE AND IT IS NOT THE SIGN — IT IS THE SCOPE, AND THE FIX IS THE
 * ONE THIS FILE ALREADY MADE ON THE OTHER SIDE OF THE LEDGER. A copied reward is a PER-CHAIN PREFIX: within one
 * family it differs between arms only by when each was forked, and the aging that is supposed to cancel it is
 * charged to the family, which makes it a common offset that cancels out of every within-family comparison
 * while the prefix spread does not. On a real page the whole frontier is one family, so the term the order was
 * actually made of was birth position and the term meant to answer it could not reach. Held on the family
 * (FlowAcct's `val`) the two are one account: every member of a family reads the SAME reward, so "members of
 * EQUAL reward" stops being the exception and becomes what a family IS, and §scheduler's guarantee holds
 * within one by construction — an arm's silence is frozen at the instant it was forked while the flow that
 * forked it goes on burning, so an unrun arm ranks at or above the flow that made it, and the queue drains
 * oldest-arm-first. AN EMISSION NO LONGER BREAKS THAT on the aging axis: it forgives the whole account's
 * window at once (FlowAcct's `emit_gen`), so the emitter and every arm of it stand level and the emitter sinks
 * again from the first quantum it burns. NOR ON THE OPTIMISM AXIS ANY MORE, which is the correction this
 * paragraph now carries: an emission does not touch `visits` at all (flow_credit_emit says why the per-member
 * zero that used to was one credit presented twice), so the emitter's count stands where its arms' frozen
 * counts stand and only COMPLETING a unit moves it. Between families the reward orders exactly what it is
 * for, charged once against a silence charged once.
 * SO THE CLAUSE THE SENTENCE ABOVE NEEDS IS "for as long as that flow does not advance its search", AND THE
 * LONGER LIST IT USED TO NEED IS RECORDED RATHER THAN QUIETLY TIGHTENED, because the sentence before that one
 * said "always" and was carried forward into a hand-off as an IDENTITY to derive a new ordering guard from —
 * which would have made the guard fire on healthy runs. The premise is true and the conclusion does not follow
 * from it: a fork does freeze the arm's `visits` and rungs at the parent's readings, and ONE writer moves the
 * parent's the other way afterwards — flow_observe_survival/flow_observe_rung raise the parent's fitness
 * monotonically while the arm's stays where it was born. That is not a defect: a flow that has just carried
 * its payload one rung further is exactly the flow the frontier should be running.
 * THERE USED TO BE A SECOND SUCH WRITER AND IT WAS THE ONE THAT MATTERED, WHICH IS WHY ITS ABSENCE IS STATED
 * HERE RATHER THAN LEFT AS A SHORTER LIST. flow_credit_emit set the emitter's `visits` to zero, so an arm
 * holding a frozen count of V was left worth `1/(1+V)` against a parent worth a full `1.0` on the same reward.
 * That reads as the fitness case — the parent did something and the arm did not — and it is a different shape,
 * because the zero was COPIED by every fork taken afterwards: two arms of one parent, forked either side of
 * one emission, were then a whole optimism range apart for an event NEITHER of them performed and BOTH of them
 * were already credited for through the account they share. That is the pair this function's own equality
 * cannot see, and it is the pair FlowAcct's `val` is held on the family to answer. The refutation is worth
 * keeping because a reader who re-derives the freezing argument will re-derive the missing quantifier with it,
 * and one who re-derives "a productive flow should not be overtaken" will re-derive the zero.
 * `cpu` IS NO LONGER ONE OF THOSE TWO, AND THAT IS THE CORRECTION THIS BLOCK CARRIES RATHER THAN A SHORTER
 * LIST. It used to be, and being one was not a bounded demotion like the visit count's — it was UNREPAYABLE.
 * The freeze is written by nothing but a dispatch, so a member the scheduler has never chosen kept its
 * parent's fork-instant burn for the life of the frontier while the parent's was forgiven at the parent's next
 * emission, and the only currency that could clear it was the dispatch the deficit itself was foreclosing.
 * The own half is now read over the ACCOUNT's silence window (FlowAcct's `emit_gen`), so a family's emission
 * forgives every arm of it at once and what the arm owes afterwards is only what the arm itself burns. The
 * FREEZE is unchanged and still correct — the arm is still worth exactly what its parent was worth at the
 * branch — and what changed is that the quantity it is a freeze OF is now one the frontier can move.
 * WHICH STATE A RUN IS IN IS STILL A MEASUREMENT rather than any sentence here: flow_wfq_census reports the
 * reward spread over the frontier and how many members have emitted anything themselves, and engine.c emits it
 * as @WFQ beside @PROGRESS and @COLD. A spread above 1.0 there is now a statement about SEVERAL FAMILIES, which
 * is a fact about the documents and the searches a session is holding rather than about one page's branching.
 *
 * AND THE JOIN IS THE WHOLE OF IT NOW — the sibling stays ATTACHED, and that one line carries both the reward
 * and the aging's family half. Where the arm enters and where the thread time it goes on to burn ends up were
 * two statements maintained separately, and for an arm that runs the rest of the document and FINISHES the
 * second answer was nowhere. They are one statement and one pointer: a fork chain is one monopolizer wearing N
 * names, so it enters at one place, it is charged as one thing, and it is CREDITED as one thing. See
 * FlowAcct. */
void flow_fork_inherit(Flow *sib, const Flow *parent) {
    DCHECK(sib != NULL && parent != NULL && sib != parent,
           "a fork's accounting was inherited from nobody, or from itself — this is the one line that decides "
           "where a newborn arm enters the queue, so it cannot be asked about one flow");
    /* NOTHING MAY HAVE BEEN CREDITED OR CHARGED FIRST, because the inheritance ASSIGNS both terms. A caller
       that ran the sibling — or credited it — before handing it its account would have run it at a rank nobody
       chose, and the assignment would then silently erase whatever that run produced. */
    DCHECK(sib->val == 0.0 && sib->cpu == 0 && sib->cpu_gen == 0 && sib->visits == 0 && sib->picks == 0 &&
           sib->cand_surv == 0.0 && sib->cand_rung == 0 && sib->path_forced == 0 &&
           sib->family == sib->acct && sib->family->fam_us == 0 && sib->family->emit_gen == 0 &&
           sib->family->base == 0.0 && sib->family->earned == 0.0 && !sib->family->placed,
           "a forked sibling was credited, charged, DISPATCHED or DECIDED before it inherited its parent's "
           "account — it was ranked, and possibly run, at a weight that belongs to no flow, and this "
           "assignment throws that away. `path_forced` is in the same list for the same reason one field over: "
           "a sibling that had already recorded a contradicted arm would have recorded it on a path it had not "
           "yet been given. `picks` is in it for the OPPOSITE reason and that is why it is worth stating: it is "
           "the one field here the assignments below deliberately do NOT carry — a dispatch is a fact about "
           "this member and never about its parent's path — so its zero is not a precondition for an "
           "inheritance but the claim that no dispatch has happened that the census would have to explain");
    /* AND IT MAY NOT ALREADY STAND UNDER A FLOW IN THE FORK TREE. flow_new mints a root node, so a non-NULL
       `up` here is a second inheritance on one sibling: the first parent's node would be leaked and every arm
       this flow later sheds would be charged to the wrong chain. */
    DCHECK(sib->acct != NULL && sib->acct->up == NULL,
           "a forked sibling already stood under a flow in the fork tree — it is being given a second parent, "
           "so the service it sheds when it departs is charged to a chain it was never part of");
    /* THE REWARD IS NOT COPIED HERE AND ITS ABSENCE IS THE MECHANISM, not an omission. It is carried by the
       family join below — one pointer, read by every arm — which is why the equality at the end of this
       function still holds over a term this function never names. A copy would be neutral only against the arm
       born at THIS instant and would put two arms of one parent, forked at two instants, at two different
       rewards for something neither of them did; see the banner and FlowAcct's `val`. `Flow.val` beside it is
       what ONE member has emitted, it is never inherited, and it ranks nothing — which is why the precondition
       above requires it to still be zero rather than assigning it. */
    sib->cpu = parent->cpu;
    /* …AND THE WINDOW THAT READING BELONGS TO, WITHOUT WHICH THE LINE ABOVE COPIES A NUMBER AND NOT A QUANTITY.
       `Flow.cpu` is the flow's burn since its ACCOUNT last emitted and is read only while `cpu_gen` matches
       that account's generation (flow_own_silence); the arm joins the parent's family below, so copying the
       mark verbatim makes the arm read EXACTLY what the parent reads — a current window if the parent's is
       current, and zero if the parent has not been charged since its family's last finding. Copying `cpu`
       alone would make an arm of a just-emitted parent read the parent's PREVIOUS window's burn as its own,
       which is the fork carrying a debt the parent had already been forgiven, and the equality at the end of
       this function is what fires on it. */
    sib->cpu_gen = parent->cpu_gen;
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
       exactly where its parent stood: the bytes that survived to a sink survived on the prefix both arms share,
       and so did the rungs those bytes reached there.
       Zeroing it would let a candidate refresh its own rank by branching — the reset-by-splitting this
       accounting forbids everywhere else, arriving through the one term that is read rather than accumulated —
       and the rank-neutrality assertion below is what catches it. For a NON-candidate parent this copies zero
       to zero, which is the truth about an arm of a flow that has no payload.
       BOTH FIELDS, AND THE SECOND ONE IS WHY THE ASSERTION BELOW IS WRITTEN OVER flow_weight. The fitness was
       ONE double when this line was written; splitting it into the fraction and the rung count is exactly the
       "a term the fork does not carry" shape, and a list-of-fields assertion would have passed with the rung
       left behind — a candidate that had ARRIVED and ESCAPED could then re-enter the queue as one that had
       merely survived, by branching, which is worth two thirds of the comparator's whole range. */
    sib->cand_surv = parent->cand_surv;
    sib->cand_rung = parent->cand_rung;
    /* AND THE FAMILY IT JOINS — the term the AGING reads AND the term the REWARD is, and the one line that
       makes "a fork chain is one monopolizer wearing N names" true of the arithmetic rather than only of this
       comment. The arm does not FOUND a family: flow_new minted it a root of its own (that is what the
       precondition above checks), and this hands that root back and points the arm at its parent's. Every
       microsecond any arm of the family burns from here on is charged once, to this node, and read by every
       arm; every finding any arm emits is credited once, to the same node, and read by every arm.
       IT CARRIES TWO OF THE EQUALITY'S TERMS AND NOT ONE, which is why this is the line the whole assertion
       below rests on. The reward used to be a field copied a few lines up, and copying is exactly what made it
       a per-chain prefix rather than the family quantity this file already called it.
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
    /* AND THE PATH'S FORCED MARK — the one thing inherited here that the ranking never reads, and it is stated
       at this line for the SAME sentence the five terms above are stated for: an arm is its parent's path with
       one more arm on it, so everything the parent's path has already stood on, the arm stands on too. A
       sibling born clean of it would declare its requests DERIVED on the strength of having been forked, which
       is the provenance equivalent of reset-by-splitting — and it is the shape the defect actually takes,
       because forcing is what MAKES most siblings: the arm minted at a contradicted branch is exactly the one
       that would deny the contradiction.
       IT IS NOT IN THE EQUALITY BELOW, and that is a statement rather than an omission: `path_forced` is not a
       weight term (flow.c's flow_mark_forced_arm says why it must never become one), so flow_weight cannot see
       it and the equality would pass whether this line existed or not. What asserts it is the pair of claims
       around this one — the precondition above that the sibling has decided nothing yet, and the equality
       below that nothing has entered the WEIGHT unnoticed. */
    sib->path_forced = parent->path_forced;
    /* §scheduler'S ONE WFQ, ASSERTED AT THE FORK: A FORK IS RANK-NEUTRAL. The sibling is the same flow's path
       with one more arm on it, so at the instant it is born it is worth exactly what the parent is worth —
       branching is neither a promotion nor a demotion. It is not a tautology dressed as a check: it fires the
       moment anything else enters flow_weight that a fork does not carry, which is precisely the shape every
       banned fix for this defect takes (a depth bonus, a script-index term, a separate visit counter, a
       "prefer flows that have run" tiebreak). Any of those would make the two sides differ here, at the fork,
       instead of six minutes later in a progress line that says `finished 0`.
       RE-DERIVED FOR EVERY TERM, AND IT IS NOT AN ASSUMPTION CARRIED OVER. flow_weight is now
       `fam_val + (cand_surv + cand_rung)/FLOW_RUNGS_N + 1/(1+visits) − (own_silence + fam_us)*RATE`, so a fork
       must carry SEVEN things for this to hold and it carries exactly seven through SIX assignments:
       `cand_surv` and `cand_rung` (copied above — the fitness comparator's two rung quantities), `visits`
       (copied above — the optimism term's), `cpu` AND `cpu_gen` (copied above — the aging's own half is a
       reading and the mark that says which window it is a reading OF, and neither is that half without the
       other), and the family tag (joined above), which carries BOTH the reward and the aging's other half. Each addition to the formula has arrived through this
       line firing, which is the whole reason it is written as an equality over flow_weight rather than as a
       list of fields. This assertion is what FORCES the join, and it now forces it twice over: had the arm kept
       the fresh root flow_new minted it, its `fam_us` would be 0 against a parent family that has burned AND
       its `val` would be 0 against a parent family that has emitted, so the sibling would be born strictly
       better on one coordinate and strictly worse on the other and this line would fire at the very first fork.
       That is the reset-by-splitting the whole of this accounting forbids, and it is caught here rather than in
       a census that says the frontier never sinks.
       AND IT IS A CLAIM ABOUT ONE INSTANT, WHICH IS THE LIMIT OF WHAT ANY ASSERTION AT A FORK CAN BE. It
       compares the arm born HERE against the parent HERE, so it cannot see two arms of the same parent forked
       at two instants — and a term COPIED at the fork instant makes exactly that pair differ, by whatever the
       parent did in between, which is birth order and not merit. That pair is why the reward is not a copied
       field: a term read through the shared root is equal across every arm at every instant, so the equality
       this line can check and the equality it cannot are the same equality. Any FUTURE term whose value can
       move between two forks of one parent is under that rule and belongs on the account, not on the flow. */
    /* …AND THE AGING'S TWO HALVES ARE STILL ONE READING FOR THE ARM, which the equality below cannot say. That
       equality compares a SUM against the parent's identical sum, so it passes whichever way the two halves
       are split — including the split the arm is born into if `cpu` is carried without `cpu_gen`, since the
       arm then reads its own half over a window that is not the family's. This is the site that mints the
       population the whole defect lived in, so the relation is asserted on the NEWBORN specifically. */
    DCHECK_AGING_ONE_WINDOW(sib, "a fork handed this arm its parent's account");
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
        DCHECKF(JS_TrampFrameCount(rt) == 0,
                "the frontier is gone and %d heap CALL FRAME(S) are still live — a suspended chain that no "
                "flow owns is unreachable memory, and every frame of it holds its locals, its closed cells "
                "and its callee. %d flow(s) were released; the last SUSPENDED one whose release freed no "
                "frame was #%d (frame=%d park=%d paged=%d started=%d). A -1 there means every suspended flow "
                "did drop its chain, so these frames were never owned by one and the leak is upstream of the "
                "frontier",
                JS_TrampFrameCount(rt), rel_i, culprit, culprit_frame, culprit_park, culprit_paged,
                culprit_started);
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
       globals; the blobs went with the flows in the loop above. Putting it in the three hosts' teardowns
       instead would be the hand-copied list build.mjs warns about — a host that forgot
       the line would leak the whole chain with nothing to say so, which is exactly how the world registry's
       own release came to be missing from one of them. It belongs to the frontier, so it goes down with it.
       THE FLOWS ARE NOT THE ONLY HOLDER, WHICH THIS USED TO SAY THEY WERE ("so this is the last of them"). An
       open @S search holds one too — solve.c's add_pending freezes the path its candidates are re-injected at,
       and that blob belongs to the SEARCH rather than to any flow, because a search outlives the flow that
       detected it and is still seedable at the next drain. Its give-back is solve_free, which solver_agent_free
       therefore calls BEFORE this function; the sentence that claimed otherwise is why that call sat after it
       and why decide_free's assert fired on every page that forked before its first tainted sink. */
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
       29550 on this fixture — and the blockedness question the preempt hook asks at every suspend point would
       then have to read a property off it instead of testing a tag. (It used to say "a length": the hook no
       longer walks and no longer reads one, and what it would read on an empty Array now is the register's own
       count of the synchronous requests it is owed — same cost, same argument, different slot.) It is minted
       by the first push. */
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
    /* `cpu_gen` beside `cpu` because the two are ONE quantity (flow_own_silence): the burn, and the account
       generation that says which silence window it is a burn in. Zero here matches the fresh account minted
       below, whose `emit_gen` is also zero, so a flow that is never charged reads a current window holding
       nothing rather than a stale mark holding whatever the field last contained. */
    f->val = 0.0; f->cpu = 0; f->cpu_gen = 0; f->visits = 0;
    /* …AND THE DISPATCH COUNT, WHICH NOTHING ELSE EVER WRITES TO ZERO. Every other quantity on this line has a
       second writer that resets it (flow_credit_emit), which is exactly why `picks` exists and exactly why its
       only zero has to be here: a member arrives never-chosen, and from then on the number only rises. */
    f->picks = 0;
    /* …AND ITS PLACE IN THE FORK TREE, minted HERE because this is the one point every flow passes through, so
       no flow can exist without a node for its arms to be charged to. A from-baseline flow's node is a ROOT
       (`up` NULL); flow_fork_inherit is the only thing that ever attaches one under another. */
    f->acct = reclaim_calloc(1, sizeof *f->acct);
    CHECK(f->acct, "flow_add: OOM allocating a flow's fork-tree node — a flow whose arms cannot be charged back "
                   "lets a fork chain burn the thread forever without its rank ever moving");
    f->acct->owner = f; f->acct->refcount = 1;
    /* …AND IT IS ITS OWN FAMILY UNTIL SOMETHING SAYS OTHERWISE. A from-baseline flow founds a family and keeps
       this; a FORK joins its parent's (flow_fork_inherit) and this root becomes a plain ancestry node. Pointed
       here rather than left NULL so no flow can exist without an account for the aging term OR THE REWARD to
       read, which is the same reason the node above is minted here. The node arrives zeroed from
       `reclaim_calloc`, so a fresh family stands at reward 0 and silence 0 — which is what the fork's and the
       arrival's preconditions both check for, and which is why neither of them has to assign a fresh one. */
    f->family = f->acct;
    g_acct_live++;
    f->cand_src = NULL; f->cand_payload = NULL; f->cand_sink = NULL;
    f->last_compiled = -1;   /* nothing compiled yet; see the no-replay DCHECK at the compile site */
    /* NOTHING INTERPOSED YET — flow.h states why the pair is what says so and why -1 is a cursor no program
       can be standing at rather than a sentinel this initialisation invents. */
    f->imm_at = -1; f->imm_next = 0;
    f->world = w;
    g_flows[g_flows_n++] = f;
    frontier_rank_changed();   /* frontier changed: the newcomer may outrank the flow holding the thread */
    return f;
}

/* THE ARRIVAL RULE — START-TIME FAIR QUEUEING'S `max{v(t), F_prev}`, AND IT IS A RELATION RATHER THAN A COPY.
 *
 * flow_fork_inherit already states half of this rule and already obeys it: "a continuation of an active flow
 * enters at that flow's virtual time, never at the system's, or any flow can reset its own virtual clock by
 * splitting." That sentence is about a FORK because a fork was the only entry anybody had looked at. It is a
 * statement about ENTERING, and the other three ways a flow enters this frontier are the ones this rule covers:
 *
 *   - engine_join_document's boot flow (engine.c) — a Document the browser handed this agent mid-run,
 *   - solve.c's candidate session — one per @S search the run seeds, and
 *   - cold.c's park_flow_add — one per recipe the tier rebuilds.
 *
 * Each of those FOUNDS a family, which is the whole of what "from-baseline" means, so it has an account of its
 * own and that account has to be placed. A flow at zero reward and zero silence carries only the optimism
 * bonus, so it entered the queue at 1.0 against a leader worth hundreds — below the entire backlog, for
 * findings it had no opportunity to make. That is the defect this rule was written for, and closing it by
 * COPYING the leader's reward onto the newcomer's account closed it for one instant only.
 *
 * WHY A COPY CANNOT BE THE ANSWER, WHICH IS THE CORRECTION THIS FUNCTION NOW CARRIES. `max(v(t), F_prev)` is
 * evaluated at every ARRIVAL, and its content is that a member which has not been SERVED does not fall behind
 * the clock. In SFQ that holds for free: the server takes the MINIMUM tag, so v(t) can never pass a waiting
 * member. THIS order takes the MAXIMUM and an emission RAISES the served account's tag, so v(t) runs AWAY from
 * the waiting members rather than toward them — and a coordinate stamped once, at creation, is passed by
 * exactly what the incumbent earns afterwards. Two newcomers created at two instants then read two different
 * coordinates for something NEITHER of them did, which is CLAUDE.md's two-instants test failed at this door,
 * one term over from where flow_optimism already failed it and for the same reason.
 * MEASURED, on the build smoke fixture and quoted at the revision it was taken at (cfbc8f48): `valMin` pinned
 * at 183.0 from the second census to the last while `valMax` gained 217 to 400.0 and the frontier grew from 89
 * to 660 members; `valZero` was 0 at every sample, so nothing was entering at the floor any more — the floor
 * was where the arrivals had been LEFT. `valArrived` froze at exactly 12 beside `cands: 12`, with `turns: 0`
 * on every one of them: the twelve were the @S candidate sessions, so the delivery probe, the derived
 * breakout, the byte-provenance distance and the fire were all unreachable BY CONSTRUCTION, and the run
 * reported a clean parked search instead of a starved one.
 *
 * SO THE COORDINATE IS READ, NOT WRITTEN, UNTIL THE ACCOUNT IS FIRST SERVED. An unplaced account's reward IS
 * frontier_vt() (acct_family_val), re-read at every pick; the dispatch that first gives it the thread freezes
 * that reading into `base` (flow_set_running) and from then on it stands on its own tag plus its own ledger
 * like every other account. That is `max(v(t), F_prev)` with the max evaluated CONTINUOUSLY for a member with
 * no F_prev to compare against, and it is what makes the placement a fact about the queue rather than a race
 * with whichever microsecond the newcomer happened to be created in.
 *
 * WHAT IT DOES NOT DO, STATED HERE BECAUSE THE NEXT READER WILL OTHERWISE ATTRIBUTE IT: it does not re-relate
 * an account that HAS been served. Such an account earns and ages on its own tag, so a leader that keeps
 * emitting still leads and a served-once account that produces nothing can be passed by it again. That is the
 * bandit ordering §scheduler asks for ("accumulated emitted VALUE ... ORDER-only"), and whether a leader's
 * advantage should decay is a different question from this one. What is closed here is the population that had
 * never been reached AT ALL.
 *
 * SO `val_min` DOES NOT CLIMB WITH `val_max` AFTER THIS, AND THAT IS A RESULT RATHER THAN A SHORTFALL — the
 * derivation is written out because the obvious next diff is inadmissible and a reader will otherwise build
 * it. A pinned floor beside a climbing ceiling can be lifted in exactly three ways and two of them break
 * flow_nonreward's bound or the ledger:
 *   (a) KEEP RE-RELATING UNTIL THE ACCOUNT EARNS. Then an account that has emitted ONCE stands at its own tag
 *       while one that has emitted NEVER stands at the clock, so a single finding DEMOTES its author by the
 *       whole reward spread — 199 points, measured — and a PROMISE outweighs a FINDING by an unbounded amount,
 *       which is precisely what flow_nonreward exists to make impossible. It is also inverted against the UCB
 *       term it would be imitating: the never-earning account here has been DISPATCHED, so it is tried and
 *       unproductive, which is the arm optimism ranks lowest.
 *   (b) RE-RELATE EVERY ACCOUNT — `max(v(t), base + earned)` for all — which has no inversion and flattens
 *       every account below the clock onto it, so `earned` orders nothing between accounts and §scheduler's
 *       reward term is a common offset. That is the erasure the base/earned split was made to be able to
 *       refuse rather than the re-relation it was made to allow.
 *   (c) MAKE THE CLOCK NOT RUN AWAY — a leader's advantage that decays, or a reward that is a rate rather than
 *       a total. §scheduler's sentence forbids the second in terms ("Additive (not a literal value/cpu
 *       ratio)"), and the first is a policy question about the LEADER and not about this door.
 * A UNIFORM lift changes no order at all (it cancels), so the handicap this rule grants is by construction a
 * promise outweighing findings — and the only principled place to stop granting it is the moment the account
 * has had the OPPORTUNITY the handicap was compensating for, which is its first dispatch. What a pinned
 * `val_min` then says is "an account that was given its turn and produced nothing stands below one that
 * produced", which is the ordering working. `val_unplaced` beside it is the row that tells that reading apart
 * from the one this rule was written for — accounts still being held at the clock because nothing has reached
 * them — and `turns` (solve.h) is where the fix is actually visible.
 *
 * IT CANNOT PROMOTE. The newcomer TIES the frontier's virtual time on the queue coordinate and stands exactly
 * one optimism range above it on the full weight — the term flow_nonreward bounds at 1.0 precisely so that "a
 * PROMISE never outweighs a FINDING" — and it pays that lift back at FLOW_AGE_QUANTUM per quantum it burns. So
 * a page that manufactures documents and candidate sessions buys each of them turns and none of them a
 * monopoly, and it buys no more of them than it did before: what changed is that the ones it made an hour ago
 * are not buried by what the leader has earned since.
 *
 * THE FUNCTION IS THEREFORE AN ASSERTION AND NOT AN ASSIGNMENT, AND THAT IS THE MECHANISM RATHER THAN A
 * SHRINKING. Four tags used to be written here — the reward, the optimism count, and both halves of the aging
 * — and each was a copy of a fact about whichever flow happened to hold the thread. The optimism count went
 * first (flow_optimism: a flow that has completed nothing reads 1.0 whoever is in service, so there was no
 * position for it to arrive at). The reward is now a reading. AND THE AGING GOES WITH THEM, WHICH IS THE
 * SECOND DEFECT AT THIS DOOR: `flow_own_silence(g_running)` and `acct_family_us(g_running)` were copied onto
 * an account that FOUNDS its own family, so the source family's identical debt was forgiven at its very next
 * emission (FlowAcct's `emit_gen`) while the newcomer's could be forgiven only by an emission of its own — a
 * debt whose only currency was the dispatch the debt itself was foreclosing, which is §scheduler's razor's
 * STARVES. It is not needed either: frontier_vt() is the serving item's QUEUE COORDINATE, aging included, so
 * copying the aging again would charge the newcomer twice for thread time it never consumed.
 */
static void flow_arrive_at_virtual_time(Flow *f) {
    DCHECK(f->val == 0.0 && f->cpu == 0 && f->cpu_gen == 0 && f->visits == 0 &&
           f->family == f->acct && f->family->fam_us == 0 && f->family->emit_gen == 0 &&
           f->family->base == 0.0 && f->family->earned == 0.0 && !f->family->placed,
           "a from-baseline flow was charged, credited or DISPATCHED before it arrived — the arrival is the "
           "absence of every one of these, so a caller that wrote one first has ranked this flow at a virtual "
           "time nobody chose, and `placed` is in the list because a stamped account stops reading the clock "
           "at all and would stand for ever at a coordinate written before it existed on the frontier");
    /* THE PLACEMENT IS THE ABSENCE OF A STAMP, so there is nothing to write and nothing to skip when the
       frontier is idle. `frontier_vt()` answers SFQ's v(0) = 0 for a session that has dispatched nothing, and
       holds the last serving coordinate between two slices, which is the case the old `if (!g_running) return`
       could not tell apart from a fresh registry. */
    DCHECK(!g_running || flow_is_member(g_running),
           "the flow holding the thread is not a member of the frontier, so the coordinate the clock is a "
           "reading of belongs to no queue — every unplaced account is being ranked against nothing");
    /* THE RULE ITSELF, ASSERTED WHERE IT IS APPLIED, AND IT IS AN EQUALITY AGAINST THE CLOCK RATHER THAN
       AGAINST THE INCUMBENT. A newcomer enters AT the frontier's virtual time — not at or below it — and
       flow_pick's STRICT comparison is what then leaves the thread with whoever holds it, so a tie is the whole
       of what "arrives" means and there is nothing left for a `<=` to express. It reads the CLOCK and not
       `flow_queue_weight(g_running)` because those are two different quantities between two dispatches: the
       item in service sinks as it burns, and frontier_vt_serve deliberately does not follow it down, since a
       member that has been given nothing may not be charged for the thread time of the one that has.
       WRITTEN OVER flow_queue_weight RATHER THAN OVER A LIST OF FIELDS for the reason flow_fork_inherit gives
       about its own equality: a list cannot fire when a term is ADDED, and every term this rule has ever been
       missing was added to the weight by somebody who did not know this function existed. A term added to the
       queue coordinate that a newcomer cannot stand at — anything read off the newcomer's own payload or its
       own history — makes the two sides differ here, at the door, instead of six censuses later in a `valMin`
       that does not move.
       TWO TERMS ARE NAMED RATHER THAN CARRIED, AND NAMING THEM IS WHAT KEEPS THIS EXACT. §@S's distance is a
       fraction OF A PAYLOAD, and a newcomer's payload is not the payload the flow in service holds — there is
       no position for it to arrive at. The OPTIMISM bonus is the same sentence one term over: it is 1/(1+units
       this flow has COMPLETED), so a flow that has completed nothing reads it at 1.0 whoever is in service.
       Both sit outside flow_queue_weight (see flow_optimism), so this equality is over the TAGS alone.
       EXACT WITH NO EPSILON, and now for a stronger reason than before: nothing is copied at all. The reward
       IS frontier_vt() by construction (acct_family_val's unplaced arm) and the newcomer's aging notch is an
       integer division of zero, so the two sides are the same float read twice. */
    DCHECK(flow_queue_weight(f) == frontier_vt(),
           "a flow arriving from the baseline did not enter at the frontier's virtual time — it was placed "
           "above the clock (a page promoting the documents and candidate sessions it creates over the whole "
           "backlog) or below it (a newcomer the ordering can never reach, which is what starved every @S "
           "candidate session behind a boot family whose account the whole frontier stood on)");
    /* AND BOTH HALVES OF THE AGING ARE STILL ONE READING — of nothing, which is what a member that has never
       been offered the thread owes. Asserted at this transition like every other one that can move either
       half, so a future edit that puts a copy back here fires at the door rather than in a census. */
    DCHECK_AGING_ONE_WINDOW(f, "a newcomer entered at the frontier's virtual time");
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

/* BLOCKED = holding an unanswered synchronous host request. COUNTED, not scanned, and the argument this line
   used to make for scanning is retired rather than deleted, because it was right about the hazard and wrong
   about one premise and a reader who re-derives it will re-introduce it. It said: "a counter is a second
   representation of the same fact, and the two drift at exactly the sites (fork, drain, release) that are
   already the hardest to keep in step. The register is short — it is one flow's outstanding requests."
   THE LAST SENTENCE IS FALSE AND IS WHAT THE REST STOOD ON. The register is not one flow's OUTSTANDING
   requests, it is one flow's whole history of parks: an entry stays until that flow delivers it, so the length
   only rises — measured at ~165 per live flow on a live document and still climbing. A per-opcode read paying
   a walk of that is the shape solver/pending_index.h records one level up, where two host doors walking these
   same registers were two thirds of the process's CPU.
   THE HAZARD IT NAMED IS REAL AND IS ANSWERED AT ITS OWN LEVEL, not waved away: the count lives ON the
   register array, so the fork and the release carry it by construction rather than by two sites remembering
   to, and it is AUDITED against a full walk at exactly those two sites — both of which already walk every
   entry, so the audit costs nothing anywhere. The third site it named, the answer, is the one that genuinely
   comes from outside, and it is why solver/pending.h has a door that is HANDED the register and a generic
   setter that crashes on that field rather than doing half the transition. See pending.h at `pending_blocked`
   for why this classification can be counted at all when the reply-side ones cannot. */
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
        /* THE ONE JOB THIS WALK TAKES THAT NOTHING WILL EVER RE-CAUSE, AND IT LEFT NO TRACE.
           HTML §7.5.10 "Destroying documents" step 7 is explicit that what it removes never runs ("remove any
           tasks whose document is document from any task queue (without running those tasks)"), and that is
           CORRECT for a routed cross-document delivery too: the Document the message was for is gone, so there
           is no listener left to fire and no page left to notice. What is not correct is doing it silently. A
           routed delivery is the single job on this queue a replay does not re-cause (see the record's WHERE
           THE WORK CAME FROM bullet in flow.h, and cold.c's park assert built on the same sentence), so from
           out here — where the only numbers are `_routedDelivered` and how many times the receiving page's own
           code ran — a delivery removed here is INDISTINGUISHABLE from one the scheduler lost, and those take
           opposite actions. It is one of the four ends solver/engine.h declares, and it is the SAME end
           core/frame/window_message.c reports when the task does get to run and finds the navigable destroyed
           (§7.5.10 step 7 stated where the task runs) — the two paths are one fact reached at two moments.
           IT IS SOUND TO NAME IT WITHOUT ASKING WHAT THE JOB IS because flow.h's sentence is what makes the
           EXTERNAL bit mean this: the routed delivery is the only work on a flow's queue that came from
           outside the replayed program. A second kind of external job would have to revisit this line, and
           that sentence is where the obligation is written down.
           COUNTED PER ARM, WHICH IS WHY THE CONSERVATION LAW IS AN INEQUALITY: a fork gives each arm its own
           Array naming the same record, so ONE queued delivery becomes an end in EACH timeline that holds a
           copy — reached here when that timeline destroys the target Document, and at the running side when it
           gets to fire. This walk is over ONE flow's queue (solver/engine.c's drop hook states why), so the
           arms are counted as they each reach it rather than swept in one pass. */
        if (mine && job_field_int(e, JOB_EXTERNAL))
            engine_routed_task_end(ROUTED_TASK_TARGET_GONE);
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
            /* AND THE OTHER REMOVAL MAY NOT TAKE A ROUTED DELIVERY AT ALL, which is a different statement from
               §7.5.10 step 7's above rather than the same one twice. That walk removes the tasks OF A DOCUMENT
               THAT IS GONE, so the delivery it takes has no page left to receive it; this one is a tracker
               coalescing its OWN queued callback (HTML §4.11.4 "The dialog element", §4.11.1 "The details
               element") in a document that is still running, and a routed delivery taken here would be a live
               page's message deleted by an element's toggle task. It cannot happen — a delivery is pushed by
               the engine and not by the runtime's enqueue, so it carries JS_TASK_HANDLE_NONE and no tracker's
               handle can name it — and that is exactly why it is asserted rather than trusted: the handle
               field is the whole of what stands between the two. */
            DCHECK(!job_field_int(e, JOB_EXTERNAL),
                   "a tracker's removal-by-handle named the §9.3.3 task a ROUTED cross-document delivery "
                   "became — that is a live document's message being deleted by an element's coalescing task "
                   "tracker, and nothing re-causes it because nothing in the replayed program caused it. A "
                   "delivery carries JS_TASK_HANDLE_NONE precisely so no handle can name it, so this is two "
                   "records wearing one name");
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

/* …AND WHETHER IT OWES AN ANSWER AT ALL — see flow.h. Scanned rather than counted, and it no longer borrows
   flow_blocked's argument for that: that one was retired above, so a cross-reference to it would send the next
   reader to a paragraph that now says the opposite. This stands on its own facts. The two halves of the
   question live in two structures this file owns outright — the operation queue, whose length is an O(1) read,
   and the flow's own token row, walked over `dyn_n` — so neither half is the register the HOST settles from
   outside, which is the thing that made a counter there both necessary and delicate. Nothing has measured this
   as hot; if something does, the answer is the same shape and the sites are all in this file. */
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
int64_t flow_service_notch(const Flow *f) { return flow_own_silence(f) / FLOW_SERVICE_US; }

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
   THE SUM IS THE SILENCE AND BOTH HALVES ARE OVER ONE WINDOW. `flow_own_silence` is what THIS FLOW has burned
   since its family last emitted and `fam_us` is what its whole fork family has burned over that same window —
   one epoch, kept by FlowAcct's `emit_gen`, which is what lets these be summed into one notch and weighed
   against one reward. The own half used to be measured since this FLOW's own last emission, a strictly longer
   window, and see `emit_gen` for the population that mismatch made unreachable. The aging is both because each
   answers a comparison the other cannot: the family half orders one fork chain against another (without it a
   family that emitted V and wears N names pays for V once and presents it N times), and the own half orders a
   family's members against each other (without it every arm reads the identical number, so on a frontier that
   IS one family — which a real page's is, every flow descending from the boot flow — the term cancels out of
   every comparison it appears in and §scheduler's monopolizer sentence has nothing left to compare).
   `int64_t` throughout: at 12 ms a quantum this counts quanta of silence, so it cannot overflow anything a
   session can reach, and it is exact — the price is applied to it, never inside it. */
int64_t flow_silence_notch(const Flow *f) {
    return (flow_own_silence(f) + acct_family_us(f)) / FLOW_SERVICE_US;
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
   EXACT IN INTEGERS AND IN FLOATS, AND NOW BECAUSE NOTHING IS COPIED AT ALL RATHER THAN BECAUSE TWO COPIES
   AGREE. This paragraph used to rest on the copies — the reward "copied verbatim at the one door that founds a
   family", `visits` "copied verbatim by both entries", the notch "an integer division of copied integers" —
   and every one of those was retired by the arrival becoming a RELATION. At a fork the reward is read through
   a POINTER the arm shares, so there is nothing to copy; at an arrival the reward is frontier_vt() read by
   both sides of the equality and the aging notch is an integer division of ZERO, so there is nothing to copy
   there either. Two flows standing at one another therefore compare EQUAL with no epsilon because they are
   reading one value twice, which is the strongest form of the property flow_fork_inherit's assert also rests
   on. */
/* EVERY TERM OF THE QUEUE COORDINATE EXCEPT THE REWARD, AND THE SPLIT IS STRUCTURAL RATHER THAN EDITORIAL.
   "the reward is the only unranged term" is the sentence every ordering assertion in this file is derived from,
   and until this split there was nowhere to ASK it: the remainder existed only as `flow_weight(f) − reward`,
   which is a SUBTRACTION of two composed doubles and therefore both imprecise (catastrophic cancellation once
   an account has accumulated) and, worse, re-derivable only by restating the formula at the asking site — the
   second copy §Architecture's auditor rule forbids, and the copy that drifts. Written as a function it is the
   SAME expression the weight is made of, so the bound below is asked of the quantity itself and a term added
   here without a range added there is a fired assert rather than a silent widening. */
static double flow_queue_nonreward(const Flow *f) {
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
    /* THE OPTIMISM TERM IS NO LONGER ONE OF THIS FUNCTION'S SUMMANDS, AND ITS ABSENCE IS THE MECHANISM. This
       function is what an ARRIVAL is placed at (flow_queue_weight, flow_arrive_at_virtual_time), and every term
       of that placement has to be a TAG a newcomer can stand at rather than a READING of what the newcomer has
       done. §@S's fitness was already excluded for exactly that sentence — "a per-payload reading is not a
       position; the payload a newcomer carries is not the payload the flow in service carries, so there is
       nothing for it to arrive at" — and the optimism bonus is the same kind of quantity one field over: it is
       1/(1+units this flow has COMPLETED), and a from-baseline flow has completed nothing whatever the flow in
       service has completed. There was no position for it to arrive at either, and the arrival copied one
       anyway. See flow_optimism below, and flow_arrive_at_virtual_time for what the copy cost. */
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
       EMITTED FINDING (FLOW_AGE_RATE); what moved is the STEP, from a point to a quantum, which is 0.012 of a
       point. The two are separate constants now precisely because one number could not be both without one of
       them being wrong.
       AN EMISSION IS WORTH AT MOST 1.0 AND NOT EXACTLY 1.0, WHICH THIS BLOCK USED TO CLAIM AND CITE AN ASSERT
       FOR. flow_credit_emit asserts `v > 0.0` and nothing more, and the @S filter-survival RATCHET credits an
       improvement in a ratio — the increment between two fractions in [0,1], which is what keeps that rung
       worth at most one point over a search's whole life. The PRICE is unaffected, because it is priced
       against the ceiling; what the wrong claim would license is an ARITHMETIC one, and it nearly did: a
       reader deriving an exactness argument from "`val` is integral" gets a bound that is off by an ulp the
       moment a reward is carried across a subtraction. That is why flow_nonreward's bound is asked of a
       quantity the reward never enters: the pairwise form it replaced subtracted two composed weights against
       two raw rewards and needed an FPU epsilon at its corner, and this one is exact there because nothing in
       it is a difference of two accumulated ledgers. A claim about a value's SHAPE is checked at the writer or
       it is not a claim.
       WHAT THAT BUYS, TERM BY TERM. A flow that finishes its unit of work inside its quantum keeps the thread
       to the end of it and hands over on its VISIT — which is the optimism term doing the fairness. A flow
       inside a unit that never ends completes no visit, so the optimism term can never demote it, and this is
       what does: it sinks 0.012 per quantum it consumes and its arms, which inherited a smaller silence, pass
       it at the first quantum rather than at the 84th. Neither term can do the other's job.
       O(1) to read: every flow points straight at its family's root, so this adds one indirection to the pick
       and no walk. It is why the preempt hook's seam assertion snapshots this notch and the visit count
       (engine.c) — between two of them a flow's weight cannot move except through an emission. */
    return -(double)flow_silence_notch(f) * FLOW_AGE_QUANTUM;
}

/* THE OPTIMISM TERM, ON ITS OWN, BECAUSE IT IS A READING OF THE FLOW AND NOT A PLACE IN THE QUEUE — the same
   split the fitness distance already sits on the far side of, and made for the same reason. §scheduler: "a UCB
   optimism bonus proportional to 1/(visits+1) so a NEVER-RUN FLOW IS NEVER STARVED". The population that
   sentence is about is a flow that has COMPLETED NO UNIT OF WORK, and the value it names for such a flow is
   1.0 — the whole range, one emission's worth of promise, which is exactly what flow_nonreward's bound prices
   it at and no more.
   IT IS NOT SOMETHING A NEWCOMER CAN INHERIT, and that is the whole content of moving it here. A FORK carries
   it and must (flow_fork_inherit: an arm has, by construction, finished every program its parent finished
   before the branch, because it IS that execution with one more arm on it). An ARRIVAL is the opposite case by
   the very predicate that routes it — flow_add_unseeded sends a flow here when it stands on NOBODY's decisions
   — so it has finished nothing, and handing it the count of whichever flow happened to hold the thread is
   handing it a fact about a stranger.
   WHAT THAT COST, MEASURED. Every from-baseline door is an arrival: the @S candidate session, a joined
   document's boot flow, a cold-resumed recipe. On a frontier whose members carried 8 to 13 completed units,
   an arriving candidate was born with a bonus of 1/9 to 1/14 instead of 1.0 — so §scheduler's never-starved
   guarantee, whose entire content is the value of this term at zero, was VOIDED at the one door it was written
   for, and voided in favour of a number belonging to a flow the newcomer had nothing to do with. Twelve
   candidate sessions stood at one coordinate for a whole run with `turns:0` on every one of them.
   AND IT IS THE TWO-INSTANTS TEST, FAILED AT THE ARRIVAL DOOR. CLAUDE.md's rule for a weight term is not "does
   a fork carry it" but "would two arms of ONE parent, forked at TWO instants, read it the same"; the same
   question at this door is whether two newcomers arriving at two instants read it the same, and copying the
   incumbent's count answered 1/9 for one and 1/14 for the other — a whole range apart, for units NEITHER of
   them completed. Read at zero for both, they do.
   ITS RANGE IS UNCHANGED AND SO IS flow_weight'S VALUE. This is the same summand it always was, moved from one
   side of the arrival split to the other; flow_nonreward adds it back and FLOW_NONREWARD_MAX still prices it
   at its reading at zero visits. What changed is only which of the two questions it belongs to. */
static double flow_optimism(const Flow *f) {
    return 1.0 / (1.0 + (double)f->visits);
}

/* THE QUEUE COORDINATE — the reward plus the remainder above, and this function is now the reward and nothing
   else, which is the whole point of the split. It is what an ARRIVAL can be placed at (flow_arrive_at_virtual
   _time), and every term of it is a TAG a newcomer copies rather than a reading of the newcomer's own payload.
   THE REWARD IS THE FORK FAMILY'S, NOT THE FLOW'S, and that is the one substantive thing this function says.
   §scheduler's reward is "accumulated emitted VALUE", and the accumulator is the same account the aging is
   charged to — one emission raises it once, one second of that account's silence gives one point of it back,
   and the two terms are then commensurate at the same scope. Read off the FLOW it was a per-chain prefix: two
   arms of one family held what their common parent held at two different instants, so they differed by an
   amount NEITHER of them earned, that difference was unbounded while every other term's range is at most 1.0,
   and the family charge that was supposed to cancel it is a common offset that cancels out of the comparison
   instead. On a page whose whole frontier is one family — which is the ordinary case, every flow descending
   from the boot flow — the order was then birth position read newest-first, and no silence at any price could
   reach the tail. See FlowAcct's `val`.
   IT IS ALSO WHY THE REWARD HAS NO RANGE AND NEEDS NONE. Every other term answers "where does this item stand
   NOW", which is a question with two ends; the reward answers "what has this account produced since the
   session began", which is a LEDGER and is unbounded by §NO BOUNDS. Bounding it would be a cap on how much a
   family may be worth. That asymmetry is exactly what flow_nonreward's bound rests on, and it is why that
   bound is one-sided. */
static double flow_queue_weight(const Flow *f) {
    return acct_family_val(f) + flow_queue_nonreward(f);
}

/* THE ONE TERM OF THE ORDER THAT IS A READING OF THE ITEM RATHER THAN A TAG ON THE QUEUE — which is why it is
   outside flow_queue_weight above and inside flow_nonreward below. There is still exactly one comparator:
   every caller ranks by flow_weight, and the two splits name two different halves of it for two different
   questions — the queue coordinate is what an ARRIVAL can be placed at, and the non-reward remainder is what a
   RANGE can be asserted over. This term is on the far side of the first split and the near side of the second.
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
   no payload has no ladder to stand on, and both fitness writers assert that rather than allowing
   one to be recorded.
   AND IT IS THE WHOLE LADDER, WHICH IS WHAT MAKES THE PARAGRAPH ABOVE TRUE OF MORE THAN ONE MOMENT. Read only
   the survival rung and the term is a constant across every candidate of any search whose page does not
   transform the payload — the fraction is 1.0 for all of them the instant their bytes surface — so "two flows
   standing at the same distance rank equal" would be the answer for a whole population that is emphatically
   not standing at the same distance. The rungs are what tell them apart, and flow.h's `cand_rung` says why
   they had to move onto the flow to do it.
   THE DENOMINATOR IS FLOW_RUNGS_N AND NOT A LITERAL, WHICH IS WHY THE LADDER COULD GAIN A RUNG BENEATH THE
   OTHERS WITHOUT ANY ARITHMETIC HERE OR IN flow_silence_us_to_sink MOVING. Both read the macro and both bound
   themselves by `FLOW_RUNGS_N - 1`, so the range stays exactly 1.0 by construction rather than by a number
   somebody re-checked. What the extra rung DOES change is what every reading is WORTH: an arrival was a third
   of the range and is now half of it, and a flow that has delivered and nothing else — which used to read 0,
   the same as a flow that had never run — reads a quarter. That rescale is order-preserving among candidates
   (every one of them that has any reading at all has delivered, so every numerator gained the same 1), and the
   ordering it newly creates is precisely the one the rung exists for. It is a comparator, held per flow and
   never accumulated, so there is no stored reading anywhere that was written under the old denominator: the
   parked-candidate record ('c', cold.c) carries the substitution and not the ladder, and a resumed candidate
   re-earns every rung by replaying to its own source read. A renumbering here is therefore not a format
   change, and it would be one the moment a rung crossed the tier. */
double flow_distance(const Flow *f) {
    DCHECK(f != NULL, "the fitness term was asked of no flow — it is a reading OF an item, so there is no item "
                      "here for it to be a reading of");
    /* THE PAIR INVARIANT — "a flow with no payload stands at exactly 0" — IS NOT ASSERTED HERE, and that is a
       decision about WHERE rather than about whether. It is transiently false BY CONSTRUCTION: an arm of a
       candidate takes its parent's account first (flow_fork_inherit decides where it enters the queue) and its
       parent's substitution a few lines later, and the rank-neutrality equality between those two points calls
       this function. So the invariant is asserted at engine.c's fork, where both halves are in place and where
       a field added to the candidate identity is already an obligation. */
    return (f->cand_surv + (double)f->cand_rung) / (double)FLOW_RUNGS_N;
}

/* THE RANGE OF EVERYTHING THE ORDER IS MADE OF EXCEPT THE REWARD — written as the DERIVATION and not as the
   number it folds to, term by term against flow_nonreward's own summands, exactly as wfq_accounted_spread is
   written against flow_weight's. Each line is one term at the end of its range that pushes the sum UP:
     - the optimism bonus is 1/(1+visits), decreasing in a count that cannot go below zero, so its maximum is
       its reading at zero visits — the value a never-run flow carries and the unit §scheduler prices one
       emission against;
     - the fitness distance is (cand_surv + cand_rung)/FLOW_RUNGS_N with cand_surv asserted in [0,1] at
       flow_observe_survival and cand_rung asserted on the ladder at flow_observe_rung, whose top is
       FLOW_RUNGS_N - 1, so its maximum is (1 + (N-1))/N = 1 exactly for ANY N — which is why the denominator
       is read here rather than a literal, and why the day a rung is added beneath the others nothing in this
       expression moves;
     - the aging enters NEGATIVELY at FLOW_AGE_QUANTUM per notch and the notch is a floor of a sum of
       microsecond counts that flow_age_running asserts non-negative, so the most it can contribute is nothing.
   IT IS ONE-SIDED, AND THAT IS §NO BOUNDS AND NOT AN OMISSION. There is no floor here because the aging is
   unbounded by design: a flow that burns the thread without emitting must be able to sink arbitrarily far, and
   a lower bound on this quantity would be precisely the clamp §scheduler forbids — "a bound truncates distinct
   work". What is bounded is how far a term may LIFT an item, because that is what decides whether a member can
   be put behind something other than a finding. */
#define FLOW_NONREWARD_MAX ( 1.0 / (1.0 + 0.0)                                          /* optimism, zero visits */ \
                           + (1.0 + (double)(FLOW_RUNGS_N - 1)) / (double)FLOW_RUNGS_N  /* distance, top rung   */ \
                           - 0.0 * FLOW_AGE_QUANTUM )                                   /* aging, no silence    */

/* WHAT THE ORDER IS MADE OF BESIDES THE REWARD, AND THE ONE PLACE THE CLAIM "THE REWARD IS THE ONLY UNRANGED
   TERM" IS ACTUALLY ASKED. Every ordering assertion in this file is derived from that claim and NONE of them
   could ask it: each was written as a comparison against a REFERENCE MEMBER whose non-reward terms all stand
   at zero, and that population empties (flow_pick says exactly when and why) — so on a frontier that has
   stopped emitting, which is the state §scheduler's guarantee is about, the claim was asserted by nothing at
   all. This is the same claim asked of ONE item, so its population is "every flow whose weight is read", which
   is non-empty whenever the scheduler is doing anything whatever. A guard that cannot be reached is not a
   weaker guard than one that can; it is the §Testing engagement ratio's empty denominator, and it reports the
   same silence as a system that is correct.
   IT SUPERSEDES THE PAIRWISE FORM RATHER THAN STANDING BESIDE IT, and the arithmetic says so rather than a
   preference. flow_pick used to assert `w(best) − w(unrun) <= R(best) − R(unrun) + 1.0` over a reference
   member `u` with every non-reward term at zero. Write N for this quantity: that inequality IS
   `N(best) − N(u) <= 1.0`, and `N(u) = 1.0 + D_u >= 1.0` for such a `u`, so it says `N(best) <= 2.0 + D_u` —
   which is this bound, weakened by `D_u` and gated on `u` existing. Nothing it caught is outside this, so
   keeping both would be the dual system §Disposition forbids, with the weaker copy the one that runs.
   WHAT IT CATCHES IS AN EDIT TO THE FORMULA, NOT A STATE OF TODAY'S FRONTIER — the three range facts the
   pairwise form named, now each checkable on its own: an optimism term whose ceiling stops being 1, a fitness
   comparator that stops being a fraction of FLOW_RUNGS_N, and an aging term allowed to go NEGATIVE (a
   "freshness" bonus, a credit for waiting, a rank-lifting term for the tail — the shape every fix for a buried
   backlog reaches for first). It also catches the shape none of them could: a term ADDED to the order that
   nobody bounded, because a new summand here with no line in FLOW_NONREWARD_MAX fires on its first non-zero
   reading.
   THE BOUND IS EXACT AT ITS CORNER AND TAKES NO EPSILON, which is what keeps it a check rather than a
   tolerance. The maximum is reached by a real member — a candidate at zero visits and zero silence whose bytes
   have SURVIVED whole and ESCAPED — and every term of it is exact in binary at that corner: `1.0/(1.0+0.0)`
   is 1.0, `(1.0 + (N-1))/N` is `N/N` for the small integral N this ladder has, and a zero notch times any
   price is zero. So the comparison is `<=` against a value the arithmetic can actually produce, and any
   epsilon added here would be room for the next unranged term to hide in — which is the same sentence the
   pairwise form's own derivation ends on.
   THE SITE IS THIS FUNCTION AND THE ADDRESS DOES NOT TRAVEL, which is §AN-ASSERT-THAT-NAMES-A-REMEDY asked
   and answered rather than skipped. The rule's test is to count the call sites that can reach the abort and to
   thread the caller's __FILE__/__LINE__ when that number is larger than one would read by hand — and the
   reason it does not bite here is not the count, it is the REMEDY: every route into this function is a request
   to RANK a flow, and the fix for a fired range is always an edit to the terms summed on the line below, in
   this file. A caller's address would name which flow was being ranked at the moment, which is not the object
   of the instruction. The tell that the rule does apply is the one absent here — a message whose remedy names
   an action with no object; this one names a place, and the place is where the crash already points. */
static double flow_nonreward(const Flow *f) {
    /* THE TWO READINGS AND THE ONE TAG, which is what this sum is now made of explicitly: the optimism
       bonus and the fitness distance are facts about THIS FLOW, and the aging is the queue coordinate it
       shares with whatever it was placed beside. flow_weight is unchanged in value. */
    double n = flow_queue_nonreward(f) + flow_optimism(f) + flow_distance(f);
    DCHECK(n <= FLOW_NONREWARD_MAX,
           "a term of the WFQ's order other than the REWARD lifted a flow further than one emission and a full "
           "fitness reading can — so the ordering is no longer made of one unranged ledger plus terms that each "
           "span at most a finding, and §scheduler's 'a never-run flow is never starved' is a claim about a "
           "quantity nobody bounded. Bound the term in flow_queue_nonreward or flow_distance, or state its "
           "range in FLOW_NONREWARD_MAX beside the three that are already stated there");
    return n;
}

/* THE COMPARATOR ITSELF, WRITTEN AS THE ONE SENTENCE EVERY ASSERTION IN THIS FILE IS DERIVED FROM: the
   account's ledger, plus everything else. It used to read `flow_queue_weight(f) + flow_distance(f)`, which is
   the same number and a worse statement — it grouped the reward with the optimism and the aging because those
   three share an ARRIVAL door, and the split the ORDER's guarantees are made along is a different one. Grouped
   this way there is exactly one place a new term can go that is not the reward, and it is bounded there. */
double flow_weight(const Flow *f) {
    return acct_family_val(f) + flow_nonreward(f);
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
   `A*Q <= val + D`. `D` is a FRACTION — `(cand_surv + cand_rung) / FLOW_RUNGS_N` with the survival rung
   asserted in [0,1] and the rung count asserted at most FLOW_RUNGS_N - 1 — so `A*Q <= val + 1` and hence
   `A <= (val + 1) / Q`. `A` is a FLOOR of `(own_silence + fam_us) / S` with `S` the cooperative quantum, so
   `own_silence + fam_us < (A + 1) * S = (val + 1) * FLOW_SILENCE_US + S`. That is this expression, and the
   caller reads the own half through flow_own_silence for the reason that function gives — the raw `Flow.cpu`
   is that quantity only while its window mark is current, and comparing a stale field against a bound derived
   from the weight is comparing two things the order never puts side by side.
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
    /* THE REWARD IN THE DERIVATION IS THE ONE flow_queue_weight READS, which is the FAMILY's — the same
       account the silence on the other side of this inequality is charged to. Read off the flow it would be a
       bound on a quantity the weight does not contain, so the guard would pass on a state the order does not
       have and fail on one it does. */
    return (int64_t)((acct_family_val(f) + 1.0) * (double)FLOW_SILENCE_US) + FLOW_SERVICE_US;
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
/* WHAT ASKING THE ORDER COSTS — see solver/flow.h's FLOW_SCANS for why the entries are counted apart, why the
   quantity is a COUNT and not a clock, and why nothing may read these. Lifetime, never reset, and `long`
   because they are compared against `steps` and `forks`, which are. */
static long g_scan_runs[FLOW_SCAN_N];
static long g_scan_weights[FLOW_SCAN_N];
long flow_scan_runs(FlowScan s) {
    DCHECK((unsigned)s < (unsigned)FLOW_SCAN_N,
           "an order-scan count was asked for an entry that is not in solver/flow.h's list — the index would "
           "read past the array, and the caller is about to publish whatever it found as a cost of this run");
    return g_scan_runs[s];
}
long flow_scan_weights(FlowScan s) {
    DCHECK((unsigned)s < (unsigned)FLOW_SCAN_N,
           "an order-scan weight count was asked for an entry that is not in solver/flow.h's list — the index "
           "would read past the array, and the caller is about to publish whatever it found as a cost of this "
           "run");
    return g_scan_weights[s];
}

static Flow *flow_pick(const Flow *seed, const Flow *exclude, int runnable_only, int worst, FlowScan why) {
    Flow *best = NULL; double bw = 0.0;
    /* THE SCAN IS ABOUT TO HAPPEN, SO IT IS COUNTED HERE AND NOT AT ITS RETURN. Every exit of this function is
       one scan performed, including the one that finds nothing, and a count taken at a return would miss
       whichever exit somebody adds next. `why` is the CALLER's, threaded in rather than derived from the four
       argument flags: two entries share a flag shape today (`flow_best` and the census read differ in nothing
       but their question), so deriving it would be a second list that agrees with the call sites only by
       luck — the same reason solver/step_unit.h's arm is assigned at each arm rather than inferred.
       CHECKED IN RANGE BEFORE IT INDEXES, and as a CHECK rather than a DCHECK for solver/cold.c's reason at
       its own histogram: the increment below is a WRITE in every build, so a dev-only guard vanishes in
       exactly the build where the store happens, and a store past the end of this array lands in whatever the
       link put after it. */
    CHECK((unsigned)why < (unsigned)FLOW_SCAN_N,
          "engine: the order was asked through a scan entry that is not in solver/flow.h's list — the value is "
          "only ever written from that enum at a flow_pick call site, so this is a cast or an uninitialised "
          "read, and the counters below would be written outside their array in every build");
    g_scan_runs[why]++;
    /* A MEMBER CARRYING THE FULL BONUS AND NO AGING — its weight is its reward + 1.0 and hence at least 1.0,
       which is the only property the assertions below need, and the test is written as exactly that premise:
       EVERY non-reward term of flow_weight at zero, read through the same functions the weight reads.
       IT USED TO BE `cpu == 0` ALONE, which was the whole test while `cpu` was the whole of both non-reward
       terms. The family charge made it two coordinates and the visit count made it three, and each time,
       testing fewer than all of them left the assertions resting on a premise the frontier stops satisfying —
       a flow at `cpu == 0` inside a family a SIBLING has since burned is worth `val + 1.0 − aging`, which can
       be deeply negative — so the guard would have started firing on healthy runs. Reading the notch rather
       than its summands is what makes this survive the next such change: the population named here is the one
       the derivation is about, however many quantities the notch is built from.
       AND IT IS THE BEST MEMBER OF THAT POPULATION RATHER THAN A BOOLEAN OVER IT, which is the difference
       between naming §scheduler's guarantee and stating it. "A never-run flow is never starved" is a claim
       about a DISTANCE — how far the best untouched member stands behind the flow the pick returns — and a
       bit can only say that such a member exists. The census makes the same distinction one level up and
       makes it deliberately: `jobs_ready` is a count and `job_w_gap` is the reading the count cannot make,
       "denominated in the order's own unit" (flow.h). The population §scheduler's sentence is actually about
       had the count and not the reading, so the guard below it could only ever restate a floor.
       AND THE POPULATION EMPTIES — WHICH IS A FACT ABOUT THIS TEST, NOT ABOUT THE FRONTIER, AND IT IS WHY THE
       RANGE CLAIM THESE GUARDS RESTED ON NOW LIVES AT flow_nonreward INSTEAD. `flow_silence_notch` reads the
       SHARED `fam_us`, so the second clause is a question about the whole FAMILY: once a family has burned one
       cooperative quantum since any of its arms last emitted, `acct_family_us` alone exceeds FLOW_SERVICE_US
       and NO member of it can answer this, whatever its own `cpu` is. On a real page the whole frontier is one
       family, so `unrun` is NULL for all of it. NOR CAN A BIRTH REFILL IT: both doors copy the incumbent's
       coordinates verbatim — a fork takes the parent's own silence and its window mark and joins its family
       (flow_fork_inherit), an arrival takes `flow_own_silence(g_running)` and its family's `fam_us` as values
       (flow_arrive_at_virtual_time) — so a
       newborn's notch and visit count are EXACTLY the incumbent's, and on a quiet frontier the incumbent is
       not in this population either. The only writer that puts the SILENCE half back is flow_credit_emit,
       which through the account's generation zeroes the own AND family silence of every member of that family
       at once. So this population is non-empty only
       within one quantum of an emission, and the two guards below — each of which short-circuits on it —
       assert NOTHING on a frontier that has gone quiet, which is precisely the frontier §scheduler's sentence
       is about. That is the §Testing empty-denominator defect standing in the guard written to catch it.
       THE ARRIVAL DOOR NO LONGER AGREES WITH THAT SENTENCE, AND THE CORRECTION IS WORTH MORE THAN THE
       PARAGRAPH IT AMENDS. The clause above says "both doors copy the incumbent's coordinates verbatim", and
       for an ARRIVAL that is now false: a from-baseline flow copies NOTHING (flow_arrive_at_virtual_time), so
       it stands at zero own silence, zero family silence and zero completed units, and it ANSWERS this test.
       Every unplaced account therefore refills this population and keeps it non-empty for as long as one of
       them stands, which is exactly the interval in which §scheduler's never-starved guarantee is a claim
       about somebody. The guards below are correspondingly LIVE where they used to short-circuit, and that is
       a forcing function rather than a result: what they assert is an identity under flow_weight as it stands,
       so they fire only on an edit that breaks the derivation — and they now get the chance to.
       WHAT IS STILL UNCOVERED IS A FRONTIER WHOSE ACCOUNTS HAVE ALL BEEN SERVED, which is the residual at the
       bottom of this function and is narrower than it was by exactly this population.
       AND THE VISIT-COUNT CLAUSE IS NOW A NARROWER TEST THAN IT WAS, WHICH IS A HONEST POPULATION AND NOT A
       WIDER HOLE. flow_credit_emit used to zero the EMITTER's `visits` as well, so within a quantum of any
       emission the emitter itself answered this test however many programs it had finished — a member that had
       just PRODUCED counted as one that had never run, which is the same flaw `never_picked` (flow.h) exists
       because of. With that write gone the clause means what it reads: a member whose own prefix has completed
       no unit of work. What that does NOT do is fix the vacuity above — the silence clause is what empties this
       set on a quiet frontier and it is untouched — so the residual below covers strictly more picks than it
       did, and it is stated there rather than left to be re-derived here. */
    const Flow *unrun = NULL;   /* the best-weighted member with EVERY non-reward term of flow_weight at zero */
    double unrun_w = 0.0;
    /* …AND THE BEST WEIGHT ANY NEVER-DISPATCHED MEMBER OFFERS, WHICH IS A DIFFERENT POPULATION AND A DIFFERENT
       QUESTION. `unrun` above needs every non-reward term at zero, which flow_credit_emit's forgiveness gives
       to members that have PRODUCED; `picks == 0` is the population §scheduler's razor is about and the only
       field in a Flow that nothing resets. Held here rather than derived at the return because the maximum is
       not known until the scan ends. */
    const Flow *never = NULL;
    double never_w = 0.0;
    DCHECK(!(seed && worst), "the eviction tail was asked with an incumbent to defend — the seed states who "
                             "keeps the THREAD, and the flow the pager gives up is not a question about that");
    /* THE SEED IS ELIGIBLE ON THE SAME TERMS AS EVERY CANDIDATE — a member of the frontier, and not one that
       has told the scheduler it can make no progress. A flow that answered OWED is the one case where the
       incumbent must NOT keep the thread: keeping it is the spin the mark exists to end. */
    if (seed && flow_is_member(seed) && !(runnable_only && flow_host_owed(seed))) {
        best = (Flow *)seed; bw = flow_weight(seed); g_scan_weights[why]++;
        if (seed->visits == 0 && flow_silence_notch(seed) == 0) { unrun = seed; unrun_w = bw; }
        if (seed->picks == 0) { never = seed; never_w = bw; }
    }
    for (int i = 0; i < g_flows_n; i++) {
        double w;
        if (g_flows[i] == exclude || g_flows[i] == seed) continue;
        /* NOT A DROP AND NOT A DEPRIORITISATION: the flow keeps its weight, its place and every work item it
           holds, and it is picked again the moment anything could have answered it (flow_clear_host_owed). */
        if (runnable_only && flow_host_owed(g_flows[i])) continue;
        /* COUNTED WHERE THE WEIGHT IS TAKEN AND NOT AT THE TOP OF THE LOOP, which is the difference between
           what the scan COSTS and how big the frontier is. The two `continue`s above skip the excluded member
           and the host-owed ones without pricing them, so a trip count would charge this scan for members it
           never weighed — and `members` on the same census already says how big the frontier was. */
        w = flow_weight(g_flows[i]); g_scan_weights[why]++;
        if (g_flows[i]->visits == 0 && flow_silence_notch(g_flows[i]) == 0 && (!unrun || w > unrun_w)) {
            unrun = g_flows[i]; unrun_w = w;
        }
        if (g_flows[i]->picks == 0 && (!never || w > never_w)) { never = g_flows[i]; never_w = w; }
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
           flow_own_silence(best) + acct_family_us(best) < flow_silence_us_to_sink(best),  /* derivation there */
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
    /* THE PAIRWISE DISTANCE THAT STOOD HERE IS GONE, AND IT MOVED RATHER THAN BEING DROPPED — see
       flow_nonreward, which asks the same thing of ONE item and therefore of every item. It asserted
       `w(best) − w(unrun) <= R(best) − R(unrun) + 1.0`, whose whole content is that everything the weight is
       made of EXCEPT the reward spans at most one emission. Written over a PAIR it needed `unrun`, and `unrun`
       is empty on exactly the frontier the claim is about (see the population's own comment above), so the
       three range facts it was built to catch — an optimism ceiling that stops being 1, a fitness that stops
       being a fraction of FLOW_RUNGS_N, an aging term allowed to go NEGATIVE — were checkable only inside one
       quantum of an emission. Asked of one item those three facts are a bound on a single quantity with no
       reference member at all, and the pair inequality follows from it by subtraction, so keeping both would
       have left a strictly weaker copy running beside the one that supersedes it.
       WHAT THE PAIRWISE FORM SAID THAT THE BOUND DOES NOT, AND IT IS NOT AN ASSERTION: that a frontier whose
       untouched members are unreachable is one whose REWARD gap is unreachable, never an aging or optimism
       question — because the inequality was tight in the reward and slack in everything else. That is now a
       consequence of flow_nonreward's bound rather than a separate check, and it is what makes the reward's
       home load-bearing: members of one family read ONE reward, so within a family the gap is identically
       zero and §scheduler's guarantee holds outright there. A gap is a gap between ACCOUNTS — two documents,
       two searches, a resumed frontier's rebuilt recipes — and the census row that says which is `families`
       beside the reward spread, with `self_emit` saying whether the leading account earned what it is ranked
       on or was placed at it by the arrival rule.

       NAMED RESIDUAL — THE TWO GUARDS ABOVE ARE STILL GATED ON `unrun`, AND THE POPULATION IS NARROWER THAN
       IT WAS BY EXACTLY THE ACCOUNTS THAT HAVE NEVER BEEN SERVED. Not covered: §scheduler's monopolizer-sinks
       sentence and its never-run floor, on a frontier more than one cooperative quantum past its last emission
       IN WHICH EVERY ACCOUNT HAS ALREADY BEEN PLACED — which is a long run's steady state once the arrivals
       have each had their first turn — AND, since the emitter's per-member visit zero went (flow_credit_emit),
       on the picks INSIDE that quantum at which the only member the silence clause admits is one that has
       completed a unit. The residual USED TO cover every quiet frontier without qualification, and the
       qualification is the whole of what the arrival becoming a relation bought here: an unplaced account
       copies nothing, so it answers the test, so the guards are live while one stands. That is a narrowing of
       this residual and it is stated rather than absorbed, because a guard that fires MORE often than it did
       must say so as surely as one that fires less — the population it covers is what its silence is a
       statement about. Both are RIGHT
       where they fire and NARROWER than the sentence they cite, which is why they crash on nothing today: the
       range claim they leaned on has moved to a guard that cannot be gated, and what is left in them is a
       claim about a PAIR, which structurally needs a reference member whose weight is known — and the only
       member whose weight is known without reading it is one at zero silence, which is the population that
       empties. What the next diff builds: a reference the frontier cannot lose — the weight a member would
       have at the family's own last emission, carried on FlowAcct beside `val` and `fam_us` so it is written
       once per family per emission and read by every arm, which makes "how far behind the emitter does the
       backlog stand" answerable with no member at zero silence anywhere. How its absence shows: identically to
       today — a frontier whose census reports members at `turns:0` while `wTop` climbs, with every ordering
       assertion in this file silent, and nothing distinguishing that from a frontier that is being served
       correctly. */
    /* §scheduler'S RAZOR, COUNTED AT THE LINE THAT DECIDES — "drops, starves, skips, reorders, or forgets ANY
       flow ... it is a CAP". STARVES is the one of the five with no instrument anywhere, and it is not a
       question a census can answer: three GAUGES over the frontier (`never_picked`, `never_picked_gap`,
       `never_picked_at_top`) say a tied tail EXISTS at some instant, and none of them can say whether a PICK
       ever passed over it. This can, because it is asked where the choice is made and about the choice.
       WHAT IT COUNTS, EXACTLY: the scheduler's own dispatch pick returned a member that has been dispatched
       before, while a member that never has stood at the SAME weight. flow_pick's comparison is STRICT and the
       incumbent is the seed, so that is the tie going to the incumbent — which flow.h is right to say must not
       be cured by relaxing the comparison (that hands the thread away at every opcode). It is counted rather
       than asserted for exactly that reason: the state is LEGITIMATE for one pick and is the razor's STARVES
       only as a rate, and a rate is a thing to measure, never a thing to abort on.
       AND IT IS THE READING MY OWN THREE CENSUS DISCRIMINATORS COULD NOT MAKE, which is why it exists. On a
       frontier that GROWS BY FORKING, `never_picked` climbing is arithmetic about the fork factor and not
       about the order (5786 flows created against 1010 dispatches — no ordering can dispatch what the thread
       has not reached); `picks_max` cannot fall toward 1 while a framed flow legitimately needs many quanta to
       finish one program; and `picks_live / P` counts BOTH re-dispatches that CONTINUE a program — necessary
       work — and re-dispatches that pass over a starved member, which are opposite things. This separates
       them: only the second is counted here.
       ONLY THE SCHEDULER'S OWN PICK. FLOW_SCAN_NEXT is engine.c's `flow_next_to_run(cur, FLOW_SCAN_NEXT)`, the
       one call whose answer becomes a dispatch; the rival scan, the host's best-weight read, the pager's tail
       and the census ask other questions and a count over them would be a number about the instrument.
       EXACT `==` AND NO EPSILON, this file's idiom: two members standing at one another read one value twice,
       and a tolerance would count members the pick can already tell apart.
       A LIFETIME COUNTER and the only kind a reader may difference — it is off the flows entirely, for
       `g_picks_total`'s reason, and reset by nothing for `g_rank_changes`'. Read it against `picksLifetime`:
       the FRACTION is what says whether the tie-break is deciding dispatches or is a rounding event. */
    if (why == FLOW_SCAN_NEXT && best && best->picks > 0 && never && never_w == bw) g_starved_picks++;
    return best;
}

/* HOW MUCH OF AN ORDER SPREAD THE CENSUS'S OWN ROWS CAN ACCOUNT FOR — one term of flow_weight per line, each
   contributing at most its own reported RANGE, because the range of a sum is at most the sum of the ranges.
   It is the bound the assertion at the end of flow_wfq_census holds the observed spread to, and it is written
   term-by-term against flow_weight rather than as a single constant so that ADDING a term to the weight
   without adding its range here is a compile-visible omission in one place and a fired assert in the other.
   READ IT AS THE DERIVATION IT IS. `val` and the fitness distance enter the weight positively and contribute
   max−min directly — the distance's floor is 0 by the type (flow.h), so its maximum IS its range. The optimism
   term is 1/(1+visits), decreasing, so its range is taken at the visit count's two ends in that order. The
   aging enters negatively at FLOW_AGE_QUANTUM per notch, and its notch is flow_silence_notch's floor of the
   SUM `(own_silence + fam_us)` while the census floors the two halves separately: floor(a+b) <= floor(a)+floor(b)+1,
   so the two reported spreads plus ONE notch bound it exactly. That +1 is the only slack in this function. */
static double wfq_accounted_spread(const WfqCensus *c) {
    return (c->val_max - c->val_min)
         + c->dist_max
         + (1.0 / (double)(1 + c->vis_min) - 1.0 / (double)(1 + c->vis_max))
         + (double)((c->svc_max - c->svc_min) + (c->svc_fam_max - c->svc_fam_min) + 1) * FLOW_AGE_QUANTUM;
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
/* THE MARK THE CENSUS COUNTS FAMILIES WITH — see FlowAcct's `census_gen`. A generation rather than a clear
   pass, for the reason the host-owed marks above are one: resetting a per-node flag over a frontier that has
   reached tens of thousands of members is a walk, and a single increment is not. */
static unsigned g_wfq_census_gen;

void flow_wfq_census(WfqCensus *out) {
    Flow *top;
    int i;
    /* THE BEST WEIGHT ANY READY JOB HOLDER OFFERS — held as a separate maximum because `job_w_gap` is a
       DIFFERENCE against a top the scan does not know until flow_best runs below, and because "no ready
       holder" must not be spelled the same way as "a ready holder at weight zero". */
    double job_w_max = 0.0;
    int have_job_holder = 0;
    /* …AND THE SAME PAIR FOR THE REPLY BACKLOG, held separately for the identical reason: `deliv_w_gap` is a
       DIFFERENCE against a top this scan does not know until flow_best runs below, and "no ready holder" must
       not be spelled the same way as "a ready holder at weight zero". */
    double deliv_w_max = 0.0;
    int have_deliv_holder = 0;
    /* THE BEST WEIGHT ANY NEVER-DISPATCHED MEMBER OFFERS, held the same way and for the same reason as the job
       holder's: `never_picked_gap` is a DIFFERENCE against a top this scan does not know until flow_best runs
       below, and "no such member" must not be spelled the same way as "one standing at weight zero". */
    double never_w = 0.0;
    int have_never = 0;
    /* …AND HOW MANY NEVER-DISPATCHED MEMBERS STAND AT THAT MAXIMUM, held beside it for the same reason the
       maximum itself is held here: whether the plateau is AT THE FRONT is a question about a top this scan
       does not know until flow_best runs below, so the width is COLLECTED here and only ATTRIBUTED to the
       front there. It costs no weighing — the walk has already computed this member's weight. */
    long never_at_w = 0;

    /* THE WEIGHINGS flow_best WILL PERFORM BELOW, READ BEFORE IT RUNS — the other half of what a sample costs,
       and it is captured here rather than derived afterwards because FLOW_SCAN_OTHER is SHARED (the host's
       best-weight read and the pager's tail run under it too), so the only way to know what THIS call spent is
       to bracket it. It is the reference the identity at the end of this function is stated against. */
    long other_before;

    DCHECK(out != NULL, "the WFQ was asked to report its ordering into nothing");
    /* THIS WALK, COUNTED — see flow.h's FLOW_SCANS for why the report's own cost is a row rather than an
       argument. Raised at the top for flow_pick's reason at its own counter: every exit of this function is one
       walk performed, and a count taken at the end would miss whichever exit somebody adds next. */
    g_scan_runs[FLOW_SCAN_CENSUS]++;
    other_before = g_scan_weights[FLOW_SCAN_OTHER];
    out->members = g_flows_n;
    out->val_min = out->val_max = out->val_top = 0.0;
    out->val_zero = out->self_emit = out->unrun = 0;
    out->val_arrived = out->val_unplaced = 0;
    /* THE CLOCK ITSELF, WHICH IS NOT A READING OF THIS WALK. It is the frontier's virtual time at the instant
       the census is taken — the coordinate every unplaced account is standing at — so it is assigned here,
       unconditionally, exactly as `nonreward_max` and `picks_lifetime` are: the same number on an empty scan
       as on a full one. Without it `valMin` and `valUnplaced` are two halves of a sentence with no subject —
       a reader cannot tell a floor that is TRACKING the clock from one that has been left behind by it. */
    out->vt = frontier_vt();
    out->never_picked = 0;
    out->never_picked_gap = 0.0;
    out->never_picked_at_top = 0;
    out->picks_live = out->picks_max = 0;
    /* THE LIFETIME HALF, WHICH IS NOT A READING OF THIS WALK AND IS ASSIGNED WHERE THAT IS OBVIOUS. It is the
       count of dispatches this instance has EVER made, so it is unconditional on the frontier the way
       `nonreward_max` is — the same number on an empty scan as on a full one — and it is the only one of the
       three pick rows a reader may difference across two censuses. */
    out->picks_lifetime = g_picks_total;
    out->svc_max = out->svc_min = out->svc_fam_max = out->svc_fam_min = 0;
    out->families = 0;
    /* A FRESH MARK FOR THIS SCAN, PAST ZERO, so a node minted since the last census (calloc'd to 0) reads as
       unmarked rather than as already counted. */
    if (++g_wfq_census_gen == 0) g_wfq_census_gen = 1;
    out->vis_min = out->vis_max = 0;
    out->cand_members = out->cand_unrun = out->cand_dec_max = 0;
    out->cand_svc_max = 0;
    out->dec_max = 0;
    out->dist_max = 0.0;
    out->w_top = out->w_min = out->cand_w_max = 0.0;
    out->top_svc = out->top_svc_fam = 0;
    out->top_forgiven = 0;
    /* THE BOUND THE GAP ROWS ARE READ AGAINST, TAKEN FROM THE MACRO flow_nonreward ITSELF ASSERTS — not a
       constant this scan chose, and not one a reader re-derives. It is unconditional because it is a property of
       the FORMULA and not of the frontier: it is the same number on an empty scan as on a full one, which is
       exactly why it can be trusted as the denominator of a reading about members. */
    out->nonreward_max = FLOW_NONREWARD_MAX;
    out->jobs_ready = out->jobs_framed = out->jobs_owed = out->vis_zero = 0;
    out->job_w_gap = 0.0;
    out->deliv_ready = out->deliv_framed = out->deliv_owed = 0;
    out->deliv_w_gap = 0.0;
    for (i = 0; i < g_flows_n; i++) {
        const Flow *f = g_flows[i];
        int64_t s = flow_service_notch(f);
        /* THE SUM THE PICK USES, not only the terms it is made of — see flow.h for the reading that went wrong
           without it. One call per member, and this scan still decides nothing. */
        double w = flow_weight(f);
        /* …AND PRICED WHERE IT IS TAKEN, exactly as flow_pick prices its own — weight EVALUATIONS and not loop
           trips, so the two rows are the same quantity and a reader may divide one by the other. This walk
           skips nothing, so its count is the frontier's size per sample by construction, which is what makes
           the identity below an assertion rather than a restatement. */
        g_scan_weights[FLOW_SCAN_CENSUS]++;
        /* HOW MANY DECISIONS THIS FLOW STANDS ON, read from wherever its decision state currently lives: a
           parked flow's blob, and decide.c's live globals for the one the scheduler is switched into — the
           same split cold.c's census makes, because there is only one place each can be. Asked of EVERY
           member and not only the candidates, because the candidates' figure is meaningless without it. */
        long dec = 0;
        /* …AND THE POSITION ALONG IT, WHICH IS A DIFFERENT NUMBER AND WAS THE ONE `cand_dec_max` CLAIMED TO
           BE. `dec` is the chain's LENGTH — decide.c's own accessor says "a caller that wants a DISTANCE
           TRAVELLED must not take this number: it is the denominator" — and an @S candidate is seeded with the
           whole detecting chain under a cursor of ZERO, so for a candidate the length is fixed from seeding
           and the cursor is the only half that moves.
           BOTH HALVES OF ONE QUANTITY, TAKEN FROM THE TWO PLACES IT LIVES. `Flow.dec_blob` is NULL exactly
           while a flow is RUNNING (engine.c frees it at the switch-in), so a reading sourced from the blob
           alone would report a real position for every parked member and ZERO for the incumbent — and that is
           wrong in the direction that LOOKS LIKE A FIX WORKING, because a number that under-reads whoever
           holds the thread makes the waiting tail appear to overtake it. The running flow's cursor is
           decide_cursor(); the same split this loop already makes one line up for the length. */
        long cur = 0;

        if (f->dec_blob) { decide_blob_stats(f->dec_blob, &dec, NULL); cur = decide_blob_cursor(f->dec_blob); }
        else if (f == g_running) { decide_live_stats(&dec, NULL); cur = decide_cursor(); }
        if (dec > out->dec_max) out->dec_max = dec;
        /* A POSITION CANNOT LIE OUTSIDE THE PATH IT IS A POSITION ON, asserted where the two are in one hand
           for the first time. Each source checks its own half — decide_blob_cursor against its blob's chain,
           decide_live_stats and decide_cursor against the live vector — and neither can see the pair this loop
           has just composed out of two calls, which is precisely where a future edit that takes the length
           from one member and the cursor from another would land. */
        DCHECK(cur >= 0 && cur <= dec,
               "the WFQ census composed a replay position outside the recorded path it is a position on — the "
               "length and the cursor were taken from one member by two calls, so this is those two calls "
               "having come to be about different flows, and every fraction built from the pair would exceed "
               "one or go negative");

        /* THE REWARD ROWS READ THE TERM THE ORDER IS MADE OF, which is the FAMILY's — a census denominated in
           a quantity the pick does not read is the shape flow.h's own paragraph describes, where a spread was
           taken as "the reward term is ordering the frontier" on a run whose top weight the spread had nothing
           to do with. Reached per MEMBER because a family has no member of its own to be asked, so a family
           with N arms contributes its reward N times to these extrema — which is right for a SPREAD (it is the
           range the pick can see across the members it ranks) and is exactly why `families` is reported beside
           them: on a one-family frontier this spread is zero by the structure, and a non-zero one is a
           statement about several accounts rather than about one page's branching.
           NO MEMBER MAY HOLD MORE THAN ITS ACCOUNT DOES, asserted where both are in one hand. `Flow.val` is
           what this ONE member emitted and every point of it was credited to its family in the same call, so a
           member standing above its own family is one whose two writers came apart — and the only symptom
           would be a `self_emit` row counting emissions the order never saw. */
        DCHECK(f->val <= acct_family_val(f) ||
               f->family == NULL,   /* departed members are not walked, but the read is stated as the pair */
               "a member of the frontier has emitted more than the fork family it is credited to holds — the "
               "two writers of one emission have come apart, so the order is ranking an account that is "
               "missing findings this member's own ledger already counted");
        if (i == 0 || acct_family_val(f) < out->val_min) out->val_min = acct_family_val(f);
        if (i == 0 || acct_family_val(f) > out->val_max) out->val_max = acct_family_val(f);
        if (acct_family_val(f) == 0.0) out->val_zero++;
        /* …AND THE POPULATION THE ROW ABOVE STOPPED NAMING THE DAY AN ARRIVAL GOT A REWARD TAG — accounts that
           have EARNED nothing, whatever coordinate they were placed at. The two were one test while a
           from-baseline flow entered at zero; flow_arrive_at_virtual_time places it at the flow in service, so
           an @S candidate session now stands at the leader's reward having emitted nothing, which is outside
           the ceiling row and is exactly the population a pinned `val_min` is about. It is one field read
           because FlowAcct's reward is two quantities — see there for why the ledger and the coordinate had to
           be separable before this question could be asked at all. */
        if (f->family && f->family->earned == 0.0) out->val_arrived++;
        /* …AND THE SUBSET OF THOSE WHOSE COORDINATE IS STILL A READING RATHER THAN A TAG, which is the one
           question that separates a candidate the order has never reached from one it has served and passed.
           Both answer `val_arrived`; only this one is still tracking `vt`, so `valArrived - valUnplaced` is
           the population that HAS held the thread and emitted nothing with it — the members a search should be
           mutated toward, rather than the members it has never been given. Read off the account for the reason
           the row above is: placement is a fact about the account, and every arm of a family reads it through
           the one pointer, so a fork cannot carry it differently. */
        if (f->family && !f->family->placed) out->val_unplaced++;
        /* …AND WHAT THIS MEMBER ITSELF PUT THERE, which is the one question the account cannot answer and the
           reason `Flow.val` exists at all. It is not a subtraction any more: nothing is inherited, so a
           member's own ledger IS what it emitted since it was born. */
        if (f->val > 0.0) out->self_emit++;
        /* HOW MANY MEMBERS STAND AT ZERO OF THE AGING'S OWN HALF — read through flow_own_silence, which is the
           quantity the order is made of, and NOT off `Flow.cpu`, which is that quantity only while its window
           mark is current. Reading the raw field was not a cosmetic difference: over the 71 censuses of the
           run this row was last measured on it was nonzero exactly TWICE, both while the frontier was under a
           thousand members, and ZERO for every census after it passed one — because past the first forks every
           member carries a nonzero inherited burn and nothing but a dispatch ever clears it, which is exactly
           the defect FlowAcct's `emit_gen` removes. */
        if (flow_own_silence(f) == 0) out->unrun++;
        /* …AND THE POPULATION THE ROW ABOVE CANNOT NAME, for the reason its own comment gives: zero own silence
           is also what an emission by ANY arm of this member's family produces, so a member that has run and
           whose account has PRODUCED is counted there. A member
           that has never been handed the thread is the population §scheduler's razor says the ordering may
           never create, and `picks` is the only field in this struct nothing resets. The best weight among
           them is carried out to the gap below rather than reported raw: how far the most-favoured starved
           member stands behind the flow the pick actually returned is the reading, and the count alone cannot
           make it (flow.h). */
        if (f->picks == 0) {
            out->never_picked++;
            /* THE MAXIMUM AND THE WIDTH OF IT, TAKEN IN ONE PASS AND WITHOUT A SECOND WEIGHING. The width
               RESETS with the maximum rather than accumulating beside it: a member that raises `never_w`
               establishes a new plateau of one, and every later member reading EXACTLY that weight joins it.
               EXACT `==` AND NO EPSILON, which is this file's established idiom rather than a shortcut — two
               members standing at one another are reading ONE value twice (flow_queue_nonreward says so of the
               arrival equality, and flow_fork_inherit's rank-neutrality check is an exact double equality over
               flow_weight), so a tolerance here would admit members the PICK can already tell apart and would
               report a plateau the ordering does not have. */
            if (!have_never || w > never_w) { never_w = w; have_never = 1; never_at_w = 1; }
            else if (w == never_w) never_at_w++;
        }
        /* …AND HOW THE DISPATCHES THAT DID HAPPEN WERE DISTRIBUTED OVER THE MEMBERS THAT GOT THEM, which is the
           question `never_picked` structurally cannot ask and which takes OPPOSITE work from the one it can.
           §scheduler's razor forbids a resume that "drops, starves, skips, reorders, or forgets ANY flow", and
           `never_picked` climbing beside a growing `members` is the tail not being reached — but it is ONE
           answer covering THREE states, which is the defect shape CLAUDE.md names in the instrument built to end
           it. A frontier of M members, P of them ever chosen, and T dispatches spent is:
             T / P near 1   the thread reached a FRESH member almost every time and the frontier is simply
                            growing faster than one thread serves it. Nothing here is an ordering defect; the
                            answer is throughput, and no weight change reaches it.
             T / P large,
             picks_max
             near T / P     a REACHABLE COHORT is being swept over and over while the tail waits — the ordering
                            is returning members it has already served ahead of members it has never served,
                            which is the state a weight term has to answer for.
             picks_max
             near T         ONE member is holding the thread and the switches are it and one rival trading —
                            a monopolizer the aging term is failing to sink, which is a different repair again.
           Two of those three take a weight change and they take DIFFERENT weight changes, and the third takes
           none at all; a report that cannot separate them is one a search cannot be directed by. `picks_live`
           is the numerator (T restricted to the members still standing) and `picks_max` is what tells the
           second state from the third; `members - never_picked` is P and is already on the same line.
           IT IS A GAUGE AND IT SAYS SO — see `g_picks_total` for why the lifetime half is a separate quantity
           held off the flows entirely, and flow.h's row for the identity that ties them. */
        out->picks_live += f->picks;
        if (f->picks > out->picks_max) out->picks_max = f->picks;
        /* THE OPTIMISM TERM'S OWN COORDINATE — completed units of work, which no service row can stand in for
           (flow.h). `vis_max == 0` on a frontier of thousands is the statement that not one member has reached
           the end of a program, and therefore that not one queued job can have run, whatever the switch and
           fork counts say. It is the number that separates "being served fairly" from "finishing nothing". */
        if (i == 0 || f->visits < out->vis_min) out->vis_min = f->visits;
        if (f->visits > out->vis_max) out->vis_max = f->visits;
        /* …AND HOW MANY MEMBERS STAND AT ZERO OF IT, which `vis_min` cannot say — see flow.h. */
        if (f->visits == 0) out->vis_zero++;
        /* THE JOB BACKLOG SPLIT BY WHAT IT IS WAITING ON — the three states flow.h names, decided by the two
           predicates the engine already asks and in the order it asks them. flow_pick refuses a host-owed
           member outright, so that question comes first; among the members the pick will consider, HTML
           §8.1.4.4 "Calling scripts"'s clean up after running script step 3 ("If the JavaScript execution
           context stack is now empty, perform a microtask checkpoint") is what every job arm of flow_step is
           under, and `frame` is that stack. Whatever is left waits on RANK, and it is the only one of the
           three the ordering can move. Written as if/else-if/else rather than three tests so the classes are
           disjoint and exhaustive BY CONSTRUCTION: a member holding jobs has exactly one reason, and a fourth
           reason has nowhere to be folded into. */
        {
            int jn = flow_job_pending(f);
            if (jn > 0) {
                if (flow_host_owed(f))   out->jobs_owed   += jn;
                else if (f->frame)       out->jobs_framed += jn;
                else {
                    out->jobs_ready += jn;
                    /* THE BEST OF THEM, against which `job_w_gap` is taken below. A maximum and not a first
                       hit: the question is how far the backlog's BEST claim stands from the front of the
                       queue, and any other holder is further still. */
                    if (!have_job_holder || w > job_w_max) { job_w_max = w; have_job_holder = 1; }
                }
            }
        }
        /* AND THE SAME THREE QUESTIONS OF THE REPLY BACKLOG — see flow.h. Asked through the delivery arm's
           OWN guard (flow_stack_empty) rather than through `frame` alone, because that is the predicate
           engine.c's arm is written against and this row exists to say whether that arm can run; the job rows
           above read `frame` because that is what THEIR arms read. Same if/else-if/else shape, so the three
           classes are disjoint and exhaustive by construction and a fourth reason has nowhere to hide.
           `pending_ready` and not `pending_deliverable_count`: the register is walked once per report by
           cold_census for the DEBT, and what this needs is only whether this member holds one. */
        if (pending_ready(f->pending)) {
            if (flow_host_owed(f))        out->deliv_owed++;
            else if (!flow_stack_empty(f)) out->deliv_framed++;
            else {
                out->deliv_ready++;
                /* THE BEST OF THEM, against which `deliv_w_gap` is taken below — a maximum and not a first
                   hit, for the reason the job holder's is: the question is how far the backlog's BEST claim
                   stands from the front of the queue, and every other holder is further still. */
                if (!have_deliv_holder || w > deliv_w_max) { deliv_w_max = w; have_deliv_holder = 1; }
            }
        }
        if (s > out->svc_max) out->svc_max = s;
        /* …AND THE FAMILY THIS MEMBER BELONGS TO, in the SAME notch — which is now the notch the AGING term
           actually reads, so `svc_fam_max` against `svc_max` is no longer a diagnostic beside the order, it is
           the order's own denominator against one member's share of it. Read per member rather than per family
           because a family has no member of its own to be asked, and the maximum over members reaches every
           family exactly as often as it has arms standing. */
        {
            int64_t fam = flow_family_notch(f);
            /* AND WHICH FAMILY THIS IS, WHICH IS THE FACT THE TWO EXTREMA BELOW CANNOT SUPPLY. `svc_fam_min ==
               svc_fam_max` is produced by two states that take OPPOSITE actions: on a frontier that is ONE
               family it is an identity of the structure — every member reads the same `fam_us` through the
               same pointer (flow_fork_inherit joins the parent's), so the equality holds whatever the run does
               and the family half can NEVER order this document's frontier; on a frontier of several families
               it is the contingent observation that one instant's service happened to agree, and the next
               charge moves it. Nothing in a pair of extrema separates "structurally an offset" from
               "currently an offset", and the second of those is a term that IS ordering and is momentarily
               level. It reads equal at a genuine split too: a from-baseline flow founds its own family but
               ARRIVES at the running family's service (flow_arrive_at_virtual_time), so two families are born
               EQUAL and diverge only once one of them is charged (flow_age_running bills the running flow's
               family alone) or credited (flow_credit_emit zeroes its own alone).
               COUNTED BY IDENTITY AND NOT BY VALUE, because two families at one service are two families. */
            DCHECK(f->family != NULL,
                   "a live member of the frontier belongs to no fork family — the aging term reads its "
                   "family's service, so this flow is charged zero for every microsecond its chain burns and "
                   "rides above every member that pays");
            if (f->family && f->family->census_gen != g_wfq_census_gen) {
                f->family->census_gen = g_wfq_census_gen;
                out->families++;
            }
            if (fam > out->svc_fam_max) out->svc_fam_max = fam;
            /* …AND ITS FLOOR, WHICH IS WHAT SAYS THE TERM IS ORDERING RATHER THAN OFFSETTING — see flow.h.
               A maximum states how LARGE the family half is; only the pair states how much of it any
               comparison in this frontier can see, and on a single-family frontier that is exactly zero. */
            if (i == 0 || fam < out->svc_fam_min) out->svc_fam_min = fam;
        }
        /* THE FLOOR, TAKEN OVER EVERY MEMBER AND NOT ONLY THE SERVED ONES — see flow.h. A frontier holding one
           never-run flow reads 0 here and that is the answer, not a hole in it: the aging term is then charging
           the rest of the frontier against a member that has burned nothing, which is exactly the case the term
           was priced for. It is `svc_max` read the other way, so it shares its scan and its unit. */
        if (i == 0 || s < out->svc_min) out->svc_min = s;
        if (i == 0 || w < out->w_min) out->w_min = w;
        /* THE FITNESS TERM'S RANGE — the fourth term of flow_weight and the last one this scan could not see.
           Its floor is 0 by the type rather than by measurement (flow.h says why), so the maximum IS the
           range. Taken over every member and not only the candidates, because the bound below is over the
           weights of every member and a candidate's distance is what lifts it above the rest of them. */
        { double d = flow_distance(f); if (d > out->dist_max) out->dist_max = d; }
        /* AND THE SAME QUESTIONS ASKED OF THE CANDIDATES ALONE — see flow.h. `cand_src` is what a candidate
           session IS (the substitution it carries), and engine.c copies it to a sibling, so this counts the
           search's whole live population rather than its roots. */
        if (f->cand_src) {
            if (!out->cand_members || w > out->cand_w_max) out->cand_w_max = w;
            out->cand_members++;
            if (flow_own_silence(f) == 0) out->cand_unrun++;   /* the order's quantity, as the row above */
            if (s > out->cand_svc_max) out->cand_svc_max = s;
            /* THE CURSOR AND NOT THE LENGTH — the correction this row waited for. Fed from `dec` it was the
               DENOMINATOR reported as the numerator: constant from the instant a candidate was seeded, so
               "how far the best of them has GOT" was a number that could not move, and the readings built on
               it named states that cannot occur. See flow.h for the three that were retired. */
            if (cur > out->cand_dec_max) out->cand_dec_max = cur;
        }
    }
    /* THE CONSERVATION IDENTITY THE PICK ROWS ARE DEFINED BY, ASSERTED RATHER THAN LEFT TO A READER. CLAUDE.md:
       a quantity whose KIND you cannot name from its output is one you are not entitled to do arithmetic on,
       and the identity that defines a counter is the one property of it a reader can actually check. Here it is
       a one-sided inequality and the side it is one-sided on IS the statement: every live member's dispatches
       were counted into the lifetime total when they happened, and the total additionally holds the dispatches
       of every member that has since departed — so the gauge can never exceed the counter, and the DIFFERENCE
       between them is exactly what the retired members took with them. Equality is the ordinary reading of a
       frontier nothing has left; a gauge ABOVE the counter would mean `Flow.picks` had acquired a writer that
       is not flow_credit_pick, which is precisely the way this instrument would come to describe a dispatch
       nobody made. It costs one comparison of two integers already in hand. */
    DCHECK(out->picks_live <= out->picks_lifetime,
           "the frontier's live members hold MORE dispatches between them than the scheduler has ever made — "
           "flow_credit_pick raises both in one statement and is the only writer of either, so a member's "
           "`picks` has been written from somewhere else and every reading derived from these rows is about "
           "dispatches that did not happen");
    top = flow_best();
    /* A NON-EMPTY FRONTIER HAS A FRONT, AND SAYING SO IS WHAT MAKES THE ROWS BELOW HONEST. flow_best is
       flow_pick with no seed, no exclusion and `runnable_only` OFF, so its loop skips nothing and takes the
       first member unconditionally — a NULL here with members standing would mean the pick had acquired a filter
       that hides part of the frontier from its own maximum. That matters more than it looks: every row derived
       from `top` is left at its zero when there is no top, and a zero silence notch is a perfectly ordinary
       reading for a leader that has just been forgiven — so "no front" and "a front at zero" would be the same
       digits, which is the defaulted-field defect performed inside the instrument that exists to find it. With
       this asserted, the zeros below can only mean an empty frontier, which result.c renders as `members: 0` and
       nothing else. The `if` stays because it is what keeps these reads out of a NULL dereference in a RELEASE
       build, where this DCHECK is compiled out — the guard and the assert are answering two different questions
       and neither replaces the other. */
    DCHECK(top != NULL || g_flows_n == 0,
           "the WFQ census walked a non-empty frontier and flow_best returned no front — flow_best takes every "
           "member with no filter, so this is the pick having gained one that hides a member from its own "
           "maximum, and every row this census derives from the top would silently read as an unforgiven zero");
    if (top) {
        /* …AND THE FRONT OF THE ORDER, READ IN THE SAME QUANTITY THE ORDER IS: the reward flow_queue_weight
           reads, which is the top flow's fork FAMILY's. `top->val` would be one member's own ledger and would
           report the front of the queue in a quantity the pick never consults. */
        out->val_top = flow_reward(top);
        out->w_top = flow_weight(top);
        /* …AND THE TWO HALVES OF THE FRONT'S OWN SILENCE, WHICH IS THE ONE THING `val_top` BESIDE THEM CANNOT
           SAY. A ledger only climbs, so a reward that has not moved between two censuses is the same digit as
           one earned slowly; the silence is what an emission RESETS, so `top_svc_fam` is the row that says
           whether the leading account's aging is being forgiven or accumulating. `top_svc` is the half that can
           actually move a difference between two members of one family — see flow.h for why the family half
           cancels there and this one does not. Read through the same notch functions the aging term reads, so
           the census and the weight cannot disagree about the unit. */
        out->top_svc = flow_service_notch(top);
        out->top_svc_fam = flow_family_notch(top);
        /* …AND HOW MANY TIMES THE FRONT'S ACCOUNT HAS HAD ITS SILENCE WINDOW FORGIVEN, which is the EVENT the
           two notches above are a reading BETWEEN. They say how much silence has accumulated since the last
           forgiveness; this says how many forgivenesses there have been, and the pair is what turns "the
           leader's aging is being reset" from an inference into a count. It is read straight off the account
           because flow_credit_emit is the only writer of that field and raises it exactly once per credit, so
           there is nothing here to derive and nothing to keep in step. See flow.h for the sweep reading it
           makes possible with `picks_max`.
           THE CAST IS NARROWING AND CANNOT LOSE ANYTHING A SESSION CAN PRODUCE: the field is uint64 for the
           reason its own comment gives (a wrap would alias a stale window mark onto a live generation), and one
           increment per emitted finding cannot reach 2^63 in any run — asserted rather than assumed, because a
           row that silently went negative would read as a leader change, which is exactly the state the series
           is meant to detect. */
        DCHECK(top->family != NULL && top->family->emit_gen <= (uint64_t)INT64_MAX,
               "the leading account's forgiveness count does not fit the row that reports it — the census would "
               "publish a NEGATIVE count, which its own reading takes for a change of leader, so a wrapped "
               "generation would be reported as a healthy front");
        /* GUARDED THE WAY ITS NEIGHBOURS ARE, BECAUSE THIS LINE WOULD OTHERWISE BE THE FIRST RELEASE-MODE
           DEREFERENCE OF `top->family` ON THIS PATH — which is a DCHECK silently promoted by an edit rather
           than by a decision. Every other row derived from the front reaches the account through an accessor
           that answers 0 for a departed node (acct_family_val, acct_family_us, flow_own_silence, each of which
           says "departed: its node is gone and so is its rank"), so the assert above is the only thing standing
           between this read and a NULL — and it is compiled out in exactly the build where the dereference
           would happen. Trading a dev abort for a release segfault is the trade that rule forbids. This is not
           an `if` past a broken invariant: the invariant is asserted one line up, where it names the site, and
           the guard states the same fact about rank the three accessors already state, so a departed front
           reports the 0 its every other row reports instead of taking the process down. */
        out->top_forgiven = top->family ? (int64_t)top->family->emit_gen : 0;
        /* AND THE ONE IDENTITY THAT DEFINES IT, WHICH IS AN EQUIVALENCE AND NOT AN INEQUALITY. flow_credit_emit
           raises `val` and bumps `emit_gen` in one function with no return between them, so an account has been
           credited exactly when its generation has advanced — in BOTH directions. A generation past zero on a
           ledger at zero is a bump from somewhere that is not an emission, and a ledger above zero at
           generation zero is a credit that never forgave the window it was supposed to; the first would erase
           the within-family order for nothing, and the second would leave a family carrying silence its own
           finding had paid for. Neither is reachable through the one writer, which is what makes this a check on
           a SECOND one. */
        /* AND THE ONE IDENTITY THAT DEFINES IT — ASKED OF THE LEDGER AND NOT OF THE ROW BESIDE IT, WHICH IS THE
           CORRECTION THIS BLOCK CARRIES AND THE REASON FlowAcct's REWARD IS TWO FIELDS. It used to compare
           against `val_top`, which is `base + earned`, on the reasoning that flow_credit_emit raises the reward
           and bumps the generation in one statement with no return between them. That reasoning is exactly
           right about `earned` and exactly wrong about the sum: a from-baseline account is PLACED at the
           virtual time of the flow in service (flow_arrive_at_virtual_time), so it stands at a reward of
           whatever the leader held — 189 points on the frontier this was written against — with `emit_gen`
           still at zero, because arriving is not emitting. Written over the sum this fires on the first census
           at which an arrived account leads, which is precisely the state a fix for the starvation would
           create: the diff that finally let a candidate session reach the front would have aborted the engine
           on an invariant that was true of the ledger and false of the field.
           WHAT IT STILL CATCHES IS THE SECOND WRITER IT WAS ALWAYS FOR, in both directions: a generation past
           zero on an account that has earned nothing is a bump from something that emitted nothing, and
           earnings on an account at generation zero are a credit that never forgave the window it paid for.
           Neither is reachable through the one writer. */
        DCHECK(top->family == NULL ||
               ((out->top_forgiven > 0) == (top->family->earned > 0.0)),
               "the leading account's EARNED ledger and its forgiveness count disagree about whether it has "
               "ever emitted — flow_credit_emit raises both in one statement and is the only writer of either, "
               "so one of them has a second writer and every reading of the front's aging is about an account "
               "whose window was opened or closed by something that emitted nothing. This is asked of `earned` "
               "and not of the reward: an account PLACED at the frontier's virtual time holds a reward it did "
               "not emit, and that is an arrival rather than a defect");
    }

    /* THE FRONT IS ONE OF THE MEMBERS THIS SCAN ALREADY WALKED, SO ITS SILENCE LIES INSIDE THE EXTREMA IT
       COLLECTED — asserted because it is the one statement that ties the two readings together. The extrema come
       from the loop above and these come from flow_best's return, which are two different traversals of what
       must be one population; a leader outside its own frontier's range is the census reading a member the walk
       never visited, or flow_best returning a departed flow, and either would make every attribution below a
       statement about a member that is not in the picture. It is not a range check on the notch functions — they
       are asked identically in both places — it is a check that the two places are asking about one set. */
    DCHECK(!top || (out->top_svc >= out->svc_min && out->top_svc <= out->svc_max),
           "the WFQ census reports the front of the order standing outside the own-silence range its own walk "
           "collected — flow_best returns a member of the frontier this scan just enumerated, so the leader "
           "cannot be outside its extrema unless the two are reading different populations");
    DCHECK(!top || (out->top_svc_fam >= out->svc_fam_min && out->top_svc_fam <= out->svc_fam_max),
           "the WFQ census reports the front of the order's FAMILY standing outside the family-silence range "
           "its own walk collected — the leader's account is one of the accounts this scan reached through its "
           "members, so a reading outside the extrema means the walk and flow_best disagree about the frontier");

    /* HOW FAR THE JOB BACKLOG STANDS BEHIND THE FRONT OF THE QUEUE — see flow.h. Taken here and not in the
       scan because it is a difference against a top the scan cannot know, and left at 0 when nothing is
       waiting on rank because there is then no gap to state; `jobs_ready` beside it is what tells the two
       apart, which is exactly why neither row is worth emitting without the other. */
    if (have_job_holder && top) out->job_w_gap = out->w_top - job_w_max;

    /* …AND THE SAME DIFFERENCE FOR THE REPLY BACKLOG, taken here for the identical reason and left at 0.0 when
       nothing is waiting on rank, so `deliv_ready` beside it is what tells "no member is waiting on the order"
       from "the front of the queue is itself holding an undelivered reply". See flow.h for the pair. */
    if (have_deliv_holder && top) out->deliv_w_gap = out->w_top - deliv_w_max;

    /* …AND THE SAME DIFFERENCE FOR THE MEMBERS THE SCHEDULER HAS NEVER CHOSEN — taken here for the identical
       reason and left at 0.0 when there are none, so `never_picked` beside it is what tells "nobody is
       starved" from "the most-starved member is standing at the front". See flow.h for the pair's reading. */
    if (have_never && top) {
        out->never_picked_gap = out->w_top - never_w;
        /* …AND THE PLATEAU IS THE FRONT'S ONLY WHEN THE BEST STARVED MEMBER IS STANDING ON IT, which is why
           this is conditioned on the exact equality and not on the gap being small. The row answers "how many
           members is the order failing to separate FROM THE FLOW THE PICK RETURNED", and a member a hair
           behind the front IS separated — the comparison can tell it apart, which is the whole question. Left
           at 0 otherwise, which is the same statement `never_picked_gap` is making with a positive value. */
        if (never_w == out->w_top) out->never_picked_at_top = never_at_w;
    }

    /* WHAT A SAMPLE COST, ASSERTED AS THE IDENTITY IT IS: this function weighs the frontier EXACTLY TWICE, once
       in its own walk and once inside flow_best, so the weighings flow_best just performed must equal the
       members this walk enumerated. It is worth an assert rather than a comment because both sides can move for
       reasons that look local and neither would say so: a filter added to flow_best would weigh FEWER (which
       the front-of-the-order assert above already worries about from the other direction, and which would make
       every gap row a comparison against a member some other member was hidden from), and a `continue` added to
       the walk here would weigh fewer still while the rows it fills silently stopped covering the frontier.
       Either way the census's price and the census's population would stop being the same number, and the row
       that exists to say what the instrument costs would be costing something else.
       IT IS ALSO THE ONE PLACE THE DOUBLING IS STATED. A reader who sees `scanCensusWeights` alone will price a
       sample at one frontier; it is two, and the second is charged to a SHARED entry (the host's read and the
       pager's tail run under OTHER too) where it cannot be told apart afterwards. Bracketing the call is the
       only way to attribute it, which is why `other_before` is taken at the top rather than reconstructed. */
    DCHECK(g_scan_weights[FLOW_SCAN_OTHER] - other_before == (long)g_flows_n,
           "the WFQ census and the flow_best inside it disagree about how many members there are to weigh — a "
           "sample is supposed to cost exactly two weighings of the frontier, so one of the two walks has "
           "acquired a filter, and the rows this census fills no longer cover the same population the order "
           "does");
    /* AND IT IS NON-NEGATIVE BY THE SAME CONSTRUCTION, ASSERTED FOR THE SAME REASON — `w_top` is flow_best's
       maximum over every member and a never-dispatched member is one of those members, read through the same
       flow_weight in the same scan. A negative deficit here would say the pick and this census have stopped
       ordering by one comparator, and this row's whole purpose is to answer whether a starved member is
       OUTRANKED or merely UNCHOSEN — which is precisely the reading a wrong sign inverts. */
    DCHECK(out->never_picked_gap >= 0.0,
           "the WFQ census reports a never-dispatched member standing ABOVE the front of its own queue — "
           "flow_best's maximum is taken over the same members this scan walks, so a negative deficit means "
           "the pick and the census are no longer reading one comparator, and the starvation row would be "
           "reporting a property of the instrument rather than of the ordering");
    /* …AND THE WIDTH IS A COUNT OVER THE POPULATION THE GAP IS A READING OF, ASSERTED SO THE TWO ROWS CANNOT
       COME TO BE ABOUT DIFFERENT SETS. Both are collected inside the ONE branch that tests `picks == 0`, so a
       width above the count is a member counted into the plateau that the population never admitted — the
       shape an edit that moves one of the two out of that branch takes, after which the pair reads as a
       plateau wider than the starved set it is supposed to be a subset of. */
    DCHECK(out->never_picked_at_top <= out->never_picked,
           "the WFQ census reports more never-dispatched members standing at the front of the order than there "
           "are never-dispatched members at all — the width and the count are collected in one branch over one "
           "population, so they have stopped being about the same set and the plateau row is a fact about the "
           "instrument");
    /* …AND THE TWO ROWS AGREE ABOUT WHETHER THE STARVED TAIL TOUCHES THE FRONT, which is the identity that
       makes the pair readable and is what gives the width a reader in every dev build. `never_picked_gap` is
       `w_top` minus the best starved weight and the width is non-zero exactly when that best weight IS
       `w_top` — one comparison written twice, so the two spellings are asserted equal rather than trusted to
       stay so. What it catches is an edit that gives either row its own notion of "at the front" (a tolerance
       added to one and not the other is the shape), after which a reader taking the pair together reads two
       questions as one. Conditioned on the population being non-empty, because both rows are left at their
       zeros when nothing is starved and a zero gap then means "nobody" rather than "at the front". */
    DCHECK(out->never_picked == 0 ||
           ((out->never_picked_at_top > 0) == (out->never_picked_gap == 0.0)),
           "the WFQ census's starvation rows disagree about whether the tail touches the front of the order — "
           "the width is non-zero exactly when the best starved member's weight IS the top, and those are two "
           "spellings of one comparison, so a reader taking the count, the gap and the width together would be "
           "reading a plateau the gap says is not there");

    /* AND IT IS NON-NEGATIVE BY CONSTRUCTION, WHICH IS WHY THIS IS AN ASSERTION AND NOT A CLAMP. `w_top` is
       flow_best's maximum over EVERY member (flow_pick with no seed, no exclusion and `runnable_only` off) and
       a ready job holder is one of those members, read through the same flow_weight in the same scan — so the
       difference cannot be negative while the pick and this census are ordering by one comparator. It fires on
       an EDIT and not on a state: a filter added to flow_best that hides a member from the maximum while
       leaving it in the frontier, or a second reading of the weight that disagrees with the pick's. Either
       would make `job_w_gap` a number about an instrument, and the row exists to answer whether a job backlog
       is an ORDERING problem — the one reading a wrong sign would invert. */
    DCHECK(out->job_w_gap >= 0.0,
           "the WFQ census reports a job backlog standing ABOVE the front of its own queue — flow_best's "
           "maximum is taken over the same members this scan walks, so a negative deficit means the pick and "
           "the census are no longer reading one comparator, and every conclusion drawn from this row about "
           "whether queued jobs are outranked or unreachable is a statement about the instrument");
    /* AND THE REPLY BACKLOG'S IS NON-NEGATIVE BY THE SAME CONSTRUCTION AND ASSERTED FOR THE SAME REASON — a
       ready delivery holder is one of the members flow_best's maximum is taken over, read through the same
       flow_weight in the same scan. The row exists to answer whether a delivery backlog is an ORDERING problem,
       which is precisely the reading a wrong sign inverts. */
    DCHECK(out->deliv_w_gap >= 0.0,
           "the WFQ census reports a reply backlog standing ABOVE the front of its own queue — flow_best's "
           "maximum is taken over the same members this scan walks, so a negative deficit means the pick and "
           "the census are no longer reading one comparator, and every conclusion drawn from this row about "
           "whether undelivered replies are outranked or unreachable is a statement about the instrument");

    /* THE FAMILY COUNT IS BRACKETED BY THE POPULATION IT PARTITIONS, asserted because both ends name a real
       break rather than a rounding. Zero families with members standing is the mark not being taken at all —
       the row would then read "one family, structurally an offset" for every frontier there has ever been,
       which is the answer that stops anybody looking. More families than members is a family counted twice,
       which means a stale generation aliased a fresh node and the count is of censuses rather than of
       families. */
    DCHECK((out->members == 0) == (out->families == 0),
           "the WFQ census counted families and members inconsistently — a frontier with members belongs to at "
           "least one family and an empty one to none, so this is the family mark not being taken, and the row "
           "that says whether the aging term's family half can order anything reads the same for every run");
    DCHECK(out->families <= out->members,
           "the WFQ census counted more fork families than there are flows to belong to them — a family is "
           "counted through its arms, so this is one node counted twice and the number is a property of the "
           "scan rather than of the frontier");

    /* THE CENSUS ACCOUNTS FOR THE ORDER IT REPORTS — the one assertion that makes this a measurement of
     * flow_weight rather than a hand-kept list of fields that used to be one.
     *
     * WHAT IT IS. `w_top - w_min` is the observed spread of the order. Every term of flow_weight contributes
     * at most its own range to that spread, and the range of a sum is at most the sum of the ranges, so the
     * spread is bounded by the ranges THIS STRUCT REPORTS. Written as that inequality — over flow_weight,
     * exactly as flow_fork_inherit's rank-neutrality equality is written over flow_weight — because what it
     * has to catch is an EDIT and not a state: the moment a term enters the weight whose range no row here
     * carries, the reported ranges stop covering the observed spread and this fires, naming the gap at the
     * census rather than in a reader's arithmetic three exchanges later.
     *
     * WHY IT HAD TO EXIST. The alternative is what stood here: a struct whose fields are added by hand and
     * emitted by a printf in another file that is also maintained by hand. `svc_min` was computed on every
     * census since it was added, described in flow.h as "the only number in this struct that can answer 'is
     * the aging term measuring this flow or the whole frontier'", and printed by nobody — a writer with no
     * reader, which §Architecture names as the mirror of the defaulted-field defect and which is harder to see
     * precisely because the value is real and asserted about. Meanwhile the aging term had grown a SECOND
     * scope (`fam_us`) whose floor no row reported at all, and the fitness term (flow_distance) had entered
     * flow_weight with no row here of any kind. Three terms, three different ways of being invisible, and the
     * census kept reporting a spread it could not account for. Measured on the smoke fixture's steady state:
     * an aging term of 856 points, of which 93.3% was the family half, over a frontier whose entire weight
     * spread was 0.020 — and not one emitted row from which a reader could tell those two numbers apart.
     *
     * WHY THE SLACK IS EXACTLY ONE NOTCH AND NOT A TOLERANCE. flow_silence_notch floors `(cpu + fam_us)` as a
     * SUM while this scan floors the two halves separately, so the term's own notch can exceed the sum of the
     * two reported notches by at most 1 (floor(a+b) <= floor(a)+floor(b)+1, exactly). That is arithmetic, not
     * a fudge factor, and it is the only slack the derivation takes: the 1e-9 beside it covers the FPU and
     * nothing else. A larger slack would be a tolerance, and a tolerance is where the next unreported term
     * would hide.
     *
     * IT DECIDES NOTHING, like the scan it closes, and it is a FUNCTION for the same two reasons
     * flow_is_min_weight is one: a DCHECK condition must be side-effect-free (this reads the struct and
     * returns a double), and because it is evaluated only inside the condition, a release build never computes
     * the bound at all. `top` NULL is an empty frontier, which has no spread to account for. */
    DCHECK(!top || out->w_top - out->w_min <= wfq_accounted_spread(out) + 1e-9,
           "the WFQ census reports an order spread wider than the term ranges it reports can account for — "
           "flow_weight has a term this census cannot see, so every reading taken from it (which term is "
           "ordering the frontier, whether the aging is measuring one flow or all of them) is a statement "
           "about an instrument rather than about the run");
}

/* The four questions, each a seed, a filter or a direction over the one scan above. */
Flow *flow_best(void) { return flow_pick(NULL, NULL, 0, 0, FLOW_SCAN_OTHER); }

/* WHICH FLOW SHOULD HOLD THE THREAD — the dispatch loop's pick, defending the incumbent on a tie. */
Flow *flow_next_to_run(const Flow *incumbent, FlowScan why) { return flow_pick(incumbent, NULL, 1, 0, why); }

/* WHO THE INCUMBENT IS DEFENDING AGAINST — the same scan with the incumbent taken OUT rather than seeded in,
   because the hook applies the strict comparison itself. Asking it with the seed would answer `cur` and the
   value yield would compare a flow against itself. */
Flow *flow_rival_of(const Flow *cur) { return flow_pick(NULL, cur, 1, 0, FLOW_SCAN_RIVAL); }

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
    Flow *tail = flow_pick(NULL, exclude, 0, 1, FLOW_SCAN_OTHER);
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

/* IS ROW `k` ONE HTML §7.5.10 "Destroying documents"' STEP 7 TAKES — the criterion, in ONE place, because the
   count below and the removal below THAT are one question asked twice. A second copy of it is the shape where
   the assert that FIRES and the mechanism that ANSWERS it disagree about which rows they are about, and that
   disagreement is silent in the direction that matters: the count would report a document clean while the
   removal had left a row behind, or the removal would take a row the count never promised. Every clause is
   argued at flow.h's declaration of the count; nothing here decides anything that declaration does not.
   IT READS THREE FIELDS AND WRITES NONE, which is what lets the removal call it DURING its own compaction: the
   compaction writes slot `w <= k` while this is asked about `k`, so a row's answer cannot change under it. */
static int prog_row_removed_by_destroy(const Flow *f, int k, uint32_t doc) {
    return f->dyn_doc[k] == doc && k > f->last_compiled && k >= f->script_i;
}

/* HOW MANY OF THOSE ROWS LIE STRICTLY BELOW `slot` — what an index recorded BEFORE the compaction has to come
   down by to name the same row after it. Asked of `imm_at`/`imm_next`, which may legitimately sit one past the
   last row (engine.c's interposition computes a base of `script_i + 1` and CHECKs it against `dyn_n`), so the
   walk is bounded by BOTH the slot and the column rather than by the slot alone. */
static int prog_removed_below(const Flow *f, int slot, uint32_t doc) {
    int n = 0;

    for (int k = 0; k < slot && k < f->dyn_n; k++)
        if (prog_row_removed_by_destroy(f, k, doc)) n++;
    return n;
}

/* See flow.h. THE CURSOR AND `last_compiled` ARE BOTH READ, and they are not the same fact: `script_i` is the
   row the flow is AT and `last_compiled` is the highest row it has ever STARTED, so between two programs the
   cursor stands one past the last started row and during one it stands ON it. A row is unstarted exactly when
   it is past BOTH — past `last_compiled` because a started row is a program this flow may be suspended inside,
   and at or past `script_i` because the cursor is where the sequence resumes. */
int flow_programs_unstarted_for_document(const Flow *f, uint32_t doc) {
    int n = 0;

    DCHECK(f != NULL, "the unstarted-program count was asked of no flow");
    DCHECK(doc != 0, "the unstarted-program count was asked about the NONE document — a handle is this "
                     "instance's index into its own name table and zero names nothing");
    DCHECK(f->script_i >= f->last_compiled,
           "a flow's cursor is BEHIND the last program it compiled — the compile site advances `last_compiled` "
           "to `script_i` and never past it, so the two have come apart and every index derived from them "
           "names a row other than the one it is about");
    for (int k = 0; k < f->dyn_n; k++)
        if (prog_row_removed_by_destroy(f, k, doc)) n++;
    return n;
}

/* See flow.h. HTML §7.5.10 STEP 7 PERFORMED ON THE ONE TASK QUEUE `JS_DropJobsForContext` CANNOT REACH — this
   flow's own program sequence. The rows go, the eight columns shrink together, and every index anything still
   holds into the sequence is brought down with them.
 *
 * THE ORDER IS FORCED AND IT IS NOT THE OBVIOUS ONE: the register is walked BEFORE the columns move, because
 * every question asked of it is which row a given entry names, and after the compaction the answer is a row
 * that has already changed identity. A removal that compacted first and fixed indices afterwards would be
 * deriving the old positions from the new ones, which is the information the compaction destroys.
 *
 * WHAT A REMOVED ROW OWNS IS GIVEN BACK EXACTLY AS flow_free GIVES IT BACK, and the two sites are written to be
 * read together: the body column hands a REFERENCE back (a program's text is shared by every timeline holding
 * it, solver/dyn_body.h), the address and token columns are this flow's own strings and are freed, and the
 * element column frees nothing because a row's `script` element is a borrowed node of a tree that outlives it.
 * A column added to the sequence is an obligation at BOTH.
 *
 * IT IS THIS FLOW'S SEQUENCE AND NOBODY ELSE'S, which is what makes the register half sound. §7.5.10 runs per
 * timeline, so a sibling arm that has not destroyed this document keeps its rows and its own naming of every
 * shared record — and the one thing this may therefore never do is WRITE a field of a record a sibling shares.
 * Dropping this register's naming of a record is a per-flow act (pending_remove gives the naming back through
 * pending_index_unref and the record survives for whoever else names it); editing the record is not. That
 * asymmetry is the whole reason the surviving-entry case below is an abort and not an arithmetic fix. */
int flow_programs_remove_for_document(Flow *f, uint32_t doc) {
    int removed, lowest = -1, w = 0;

    DCHECK(f != NULL, "§7.5.10 step 7's program removal was asked of no flow");
    DCHECK(doc != 0, "§7.5.10 step 7's program removal was asked about the NONE document — a handle is this "
                     "instance's index into its own name table and zero names nothing");
    /* THROUGH THE COUNT, so the removal cannot run over a cursor pair the count would have refused: that
       function asserts `script_i >= last_compiled`, which every index below is derived from. */
    removed = flow_programs_unstarted_for_document(f, doc);
    if (removed == 0) return 0;
    for (int k = 0; k < f->dyn_n; k++)
        if (prog_row_removed_by_destroy(f, k, doc)) { lowest = k; break; }
    DCHECK(lowest >= 0,
           "§7.5.10 step 7's removal counted rows to take and then could not find one — the count and this "
           "walk ask prog_row_removed_by_destroy over the same column with the same document, so they cannot "
           "disagree unless something wrote the sequence between them");
    /* THE WHOLE OF WHY THE CURSOR PAIR SURVIVES THIS UNTOUCHED, ASSERTED RATHER THAN ARGUED — `lowest` is the
       SMALLEST index going, so this one comparison establishes that NO removed row lies below `script_i` or at
       or below `last_compiled`, and therefore that neither of them can name a different row afterwards. It is
       an assert and not a comment because it is the claim a later reader is most likely to break: widen the
       criterion by one clause — take a started row, take a row at the cursor — and every index in this file
       silently renames its referent while staying perfectly in range, which is §AN-INDEX-NAMES-A-THING-ONLY-
       WHILE-THE-SET-IS-FIXED with nothing to say it happened. The criterion is what holds this, so this fires
       the moment the criterion stops holding it. */
    DCHECK(lowest > f->last_compiled && lowest >= f->script_i,
           "§7.5.10 step 7's removal is about to take a row at or below this flow's cursor — `script_i` and "
           "`last_compiled` are ABSOLUTE positions this walk deliberately does not repair, and they are only "
           "safe because every removed row sits above BOTH. A row taken below either one leaves the cursor "
           "naming a different program than the one it stood at, with every index still in range and nothing "
           "anywhere to say so. If the criterion is meant to reach these rows, the cursor pair has to be "
           "brought down with them here, exactly as `imm_at`/`imm_next` are below");

    /* THE ENTRIES THAT NAME A ROW, FIRST. engine_pending_docscript records the row's ABSOLUTE position on the
       register (PEND_SCRIPT_I) and flow_deliver_one_reply writes the fetched source into `f->dyn[scriptI]`, so
       the register is the one structure outside this column that holds an index into it.
       AN ENTRY WHOSE ROW IS GOING LEAVES WITH IT, and that is what §7.5.10 step 2's "Abort document." reaches
       — HTML §7.5.11 "Aborting a document load", whose own step 2 is "Cancel any instances of the fetch
       algorithm in the context of document, discarding any tasks queued for them, and discarding any further
       data received from the network for them". The reply has nowhere to land once the row is gone, and a
       reply that DID land would be a program of a destroyed Document compiled into a realm whose browsing
       context is null, which is the exact thing step 7 exists to prevent. Walked BACKWARDS because
       pending_remove is a swap-remove: it moves the LAST entry into the hole, and descending means that entry
       has already been visited.
       AN ENTRY WHOSE ROW SURVIVES BUT SHIFTS CANNOT BE FIXED HERE, AND THE ABORT IS THE HONEST ANSWER RATHER
       THAN A GAP. Bringing its slot down is a WRITE to a record a forked sibling shares (pending.h: a fork
       shares records, and solver/pending_index.h states it outright: one record is one member however many
       flows name it), and that sibling has not destroyed this document and has not moved a row: the same field
       would have to hold two different positions at once. `pending_unshare` is not the way out either — its own
       contract admits a copy only when no reply is coming for the original, and an outstanding document script
       is precisely a record a reply IS coming for. The engine already refuses the OTHER shifting operation for
       the same reason one screen into engine_queue_into's IMMEDIATE arm, and this is that assert's twin. */
    for (int k = pending_count(f->pending) - 1; k >= 0; k--) {
        JSValue e = pending_entry(f->pending, k);
        int docscript = (int)pending_get_int(e, PEND_KIND) == FLOW_PENDING_DOCSCRIPT;
        int slot = (int)pending_get_int(e, PEND_SCRIPT_I);

        JS_FreeValue(pending_ctx(), e);
        if (!docscript) continue;
        DCHECK(slot >= 0 && slot < f->dyn_n,
               "an external document script's park names a sequence position this flow does not have — the "
               "entry is pushed with the slot it was queued at and nothing but this removal moves one, so the "
               "park and the column have come apart");
        if (prog_row_removed_by_destroy(f, slot, doc)) { pending_remove(&f->pending, k); continue; }
        DCHECK(slot < lowest,
               "§7.5.10 step 7 is removing a destroyed Document's programs from under an OUTSTANDING external "
               "script of a DIFFERENT document — that park names its row by ABSOLUTE position, the row is "
               "about to move down, and the position cannot be corrected because the record is SHARED with "
               "every forked arm and only this arm destroyed the document. BUILD ROW IDENTITY: give each row "
               "of the sequence a per-flow id that a fork copies and a removal does not reuse, have "
               "engine_pending_docscript record THAT instead of the position, and have flow_deliver_one_reply "
               "find the row by it — then no arm's removal can rename another arm's row, and this abort and "
               "engine_queue_into's interposition twin both go");
    }

    /* THE INTERPOSITION WITNESS IS THIS FLOW'S OWN AND IS SHARED WITH NOBODY, so it is ARITHMETIC where the
       register above is an abort. `imm_at` is the base slot a run of §4.12.1.1 "Processing model"'s immediate
       programs was computed against and `imm_next` is the slot the next one takes; both are absolute, both sit
       at or above the cursor, and every removed row is at or above the cursor too — so both can move.
       A RUN WHOSE BASE SLOT IS ITSELF REMOVED HAS NO RUN LEFT: the witness is what tells a SECOND interposition
       that it belongs behind the first, and with the program that opened the run gone there is no first. It
       goes back to the "no run is open" pair rather than to a corrected number, because a corrected number
       would claim a run that nothing is standing in. */
    if (f->imm_at >= 0) {
        if (f->imm_at < f->dyn_n && prog_row_removed_by_destroy(f, f->imm_at, doc)) {
            f->imm_at = f->imm_next = -1;
        } else {
            f->imm_next -= prog_removed_below(f, f->imm_next, doc);
            f->imm_at   -= prog_removed_below(f, f->imm_at, doc);
        }
    }

    /* AND THE COLUMNS, COMPACTED IN PLACE. `w` never runs ahead of `k`, so the predicate above is always asked
       about a slot no write has reached yet — which is why the criterion may be re-asked here instead of
       recorded into a side table this would then have to allocate and could then fail to. */
    for (int k = 0; k < f->dyn_n; k++) {
        if (prog_row_removed_by_destroy(f, k, doc)) {
            /* A ROW THAT OWES AN ANSWER MAY NOT SIMPLY VANISH. A non-NULL token is a peer instance suspended on
               this row's completion (flow.h says what a CROSS_AGENT_OP row without one is: "a peer suspended
               forever"), and dropping the row reaches that same state by the other door — the peer waits for
               the rest of the session and nothing anywhere names what it is waiting for. The token is still
               freed below, because in release there is nothing better to do with it than not leak it. */
            DCHECK(f->dyn_token[k] == NULL,
                   "§7.5.10 step 7 removed a destroyed Document's program that still OWED A CROSS-AGENT "
                   "ANSWER — a peer instance is parked on this row's rendezvous token and the destruction "
                   "would leave it suspended for the rest of the session. The destruction has to ANSWER it "
                   "first, with the completion a destroyed document gives, before the row may go");
            dyn_body_unref(f->dyn[k]);
            free(f->dyn_token[k]);
            free(f->dyn_url[k]);
            continue;
        }
        if (w != k) {
            f->dyn[w]       = f->dyn[k];
            f->dyn_cand[w]  = f->dyn_cand[k];
            f->dyn_type[w]  = f->dyn_type[k];
            f->dyn_url[w]   = f->dyn_url[k];
            f->dyn_el[w]    = f->dyn_el[k];
            f->dyn_doc[w]   = f->dyn_doc[k];
            f->dyn_token[w] = f->dyn_token[k];
            f->dyn_pos[w]   = f->dyn_pos[k];
        }
        w++;
    }
    DCHECK(w == f->dyn_n - removed,
           "§7.5.10 step 7's compaction kept a different number of rows than the count said it would take — "
           "the count and the walk ask one predicate over one column, so a disagreement means the sequence "
           "was written between them and the eight columns no longer describe one queue");
    f->dyn_n = w;
    /* `script_i` AND `last_compiled` ARE DELIBERATELY UNTOUCHED — the proof is the `lowest` assert above, which
       is where it is checkable: there the columns still hold the positions the claim is about. */
    DCHECK(flow_programs_unstarted_for_document(f, doc) == 0,
           "§7.5.10 step 7 removed a destroyed Document's queued programs and the document still has some — "
           "the compaction is over the same predicate the count is, so a survivor means a row moved ACROSS the "
           "cursor and became unstarted, which nothing in this walk can do");
    return removed;
}
