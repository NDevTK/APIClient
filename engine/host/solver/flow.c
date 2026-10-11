/* Flow registry and WFQ (flow.h). The registry also owns the teardown of a flow that never finished: the fields are
 * declared in flow.h, so their destructors belong where a Flow is freed, rather than in a scheduler that must
 * remember to sweep the survivors before the registry goes down. */
#include "solver/flow.h"
#include "solver/engine.h"     /* ENGINE_QUANTUM_MS: the scheduler's slice is this file's service quantum */
#include "solver/cow.h"        /* a survivor's heap COW delta */
#include "solver/dom_cow.h"    /* …and its DOM delta head + shared base segment */
#include "solver/decide.h"     /* …and its suspended decision vector */
#include "solver/concolic.h"   /* …and its suspended path constraint */
#include "solver/cold.h"       /* …and the park document written out of all of them */
#include "solver/reclaim.h"   /* the engine's own allocations ask for a flow back before they fail */
#include "solver/world.h"    /* …and whether two sending timelines it committed to can both be true */
#include "check.h"
#include <stdlib.h>
#include <string.h>    /* memset — the census's residue map is cleared per sample, never stamped */

static Flow **g_flows = NULL;
static int g_flows_n = 0, g_flows_cap = 0;
static long g_rank_changes = 0;   /* the rank-change event counted for the life of the instance, commensurable
                                     with the scan counters, which nothing resets either (see
                                     frontier_rank_changed for why the generation number is not) */
static unsigned g_gen = 0;   /* bumped whenever the frontier's membership changes (add/remove) — lets the
                                value-yield recompute the rival only on a change, never per-opcode */
/* Every row ever appended to any flow's commitment ledger for the life of this frontier: a lifetime counter, kept
   here rather than on a Flow because a sum over the standing members is a gauge that falls when a receiver departs,
   and the question is what flows that have gone wrote. Declared at the top because flow_registry_free, which resets
   it beside the ask census it is read against, stands above the ledger's block. flow.h has the three states it
   separates; flow_world_commit_push is its one raise. */
static long g_world_commit_rows;
/* Every dispatch this instance has made: a lifetime counter, kept here because `Flow.picks` is a gauge once a member
   departs (its number leaves with it). The pair separates "each dispatch reached a fresh member" from "dispatches
   land on members that already had one". Not reset by flow_registry_init, for `g_rank_changes`' reason: a ratio of
   one lifetime to a lifetime that restarted is about no run. Cross-checkable: flow_credit_pick has one caller and
   engine.c raises `g_switches` beside it, so this equals the result document's `_switches`. */
static int64_t g_picks_total = 0;
/* The half of that total the frontier no longer holds, which makes the total a partition: the members' gauge sum
   plus this equals `g_picks_total`, asserted at the end of flow_wfq_census and published as `picksDeparted`
   (`WfqCensus.picks_departed`), so flow.h's reading `(picks_lifetime - picks_live) / departures` is checked rather
   than assumed. Raised at flow_remove, two lines from the departure count by necessity (the departure is counted
   after `free(f)` and this reads `f->picks`); an early return between them fires the identity. Not reset by
   flow_registry_init: the drain through flow_remove credits every member, so the identity reads `0 + total ==
   total` at the next init. It decides nothing: flow_weight does not read it and no fork carries it
   (flow_fork_inherit asserts a newborn arm's `picks` is 0). */
static int64_t g_picks_departed = 0;
/* How many of those dispatches reached a member whose JavaScript execution context stack was empty: the lifetime
   half of the census's `mem_unframed`, which as a gauge cannot answer it (a member dispatched unframed that then
   framed itself, or departed, has left the gauge). Raised in flow_credit_pick in the same block as `g_picks_total`,
   so the containment asserted at flow_wfq_census holds by construction rather than by a numerator raised at a
   selector over a denominator raised at a switch. Asked through flow_stack_empty, the predicate the census's arms
   use (HTML §8.1.4.4 "Calling scripts"' clean up after running script step 3), so one edit reaches numerator and
   population together. Not reset by flow_registry_init. It decides nothing and moves no rank: a count of dispatches
   a population did not receive is the numerator a watchdog would be built from, and none may be. */
static int64_t g_unframed_picks_total = 0;
/* The subset of those that reached a member the census's ready arm counts (`!flow_host_owed && flow_stack_empty &&
   flow_job_pending > 0`), which makes `unframed_picks_lifetime` a measurement of the job backlog rather than a
   bound. Raised inside the superset's `if`, so the containment holds by construction and flow_job_pending's property
   read is paid once per unframed dispatch. flow.h's identity `jobs_ready == (jobs / members) * mem_unframed` holds
   only for a frontier that has run no job (a fork byte-copies its parent's queue) and fails after the first, which
   is why this row exists. It decides nothing, under the same ban. */
static int64_t g_ready_picks_total = 0;
/* The one clock both of a flow's task carriers are stamped from, which lets flow_step choose between them without a
   preference. HTML §8.1.7.1 "Definitions" partitions work by source ("For each event loop, every task source must
   be associated with a specific task queue"), while a flow's arrays partition it by carrier, a program (`dyn`) or a
   queued C callback (`jobs`); core/timing/task_source.h states the cost and one repair. Arrival order is the other:
   within one source arrival order is queue order, so a source on two carriers is ordered exactly over arrival, and
   one clock discharges §8.1.7.1 for every source, including ones no producer has written yet. It also makes
   non-starvation structural: HTML §8.1.7.3 "Processing model" step 2.1 leaves the queue choice
   "implementation-defined", and §8.1.7.1's example bounds that freedom ("keeping the interface responsive but not
   starving other task queues"). A fixed arm order starves when the arm above is fed by a set the page extends,
   while an item's stamp never changes and the items older than it are finite and fixed at its birth: an order, not
   a budget. It starts at 1, so 0 (`dyn_id`'s default) names no item. It is not parked: the cold tier stores a recipe
   and replays, so a resumed flow's rows and callbacks are rebuilt and restamped together. */
static uint64_t g_work_seq = 1;

uint64_t flow_work_seq_next(void) {
    /* The stamp is carried as a JS number on the job record, so it is only a name while it round-trips: past 2^53
       two items would be ordered by a number that is neither one's arrival. A DCHECK, since it is this engine's own
       arithmetic. */
    DCHECK(g_work_seq < 9007199254740992ull,
           "this instance has queued more work items than a JS number names exactly — the stamp is stored on "
           "the job record as a number and read back to order it against a program row, so past this point two "
           "items compare by a value that is neither one's arrival");
    return g_work_seq++;
}
/* Every member this instance has ever admitted to the frontier and every one it has let go: lifetime counters that
 * say whether the order is deciding anything. `g_rank_changes` is not this count: frontier_rank_changed is called
 * at the clock write (frontier_vt_serve), the fitness observations (flow_observe_replay/_survival/_rung), the
 * completed-unit credit (flow_credit_visit), the host-owed transitions (flow_set_host_owed, flow_clear_host_owed,
 * flow_clear_host_owed_all), the arrival (flow_new) and the departure (flow_remove), and only the last two change
 * membership; on a page that emits nothing and never moves the clock it happens to track arrivals closely, which
 * is the agreement that never gets checked.
 *
 * `arrivals / picksLifetime` is how many members are minted per dispatch. Below 1 the frontier is draining and the
 * order decides who is served first, which is what flow_weight's terms argue about; above 1 the served prefix is a
 * vanishing fraction of a growing set, and separating two members of the untouched remainder reorders something
 * nothing consumes. It is not a bound and licenses none: it says which reading a `neverPickedAtTop` plateau has.
 * `members / picksLifetime` is a gauge over a counter, a holding ratio that cannot be differenced into a rate.
 * Before calling an ordering defect, read `picksLifetime` beside `neverPicked` (a fork factor fills a frontier no
 * thread has reached) and `starvedPicksIdle` against `picksLifetime`, the one instrument that asks whether a pick
 * passed over a member; `neverPickedGap` 0 means the best never-picked member stands at the top. An ordering defect
 * is repaired by a term and a throughput one by finding where a turn goes.
 *
 * Asking the order costs O(members) per ask, one ask per step and about two per fork (the rival cache's key is a
 * disjunction of the generation and the incumbent; `rivalMissGen`, `rivalMissCur` and `rivalMissBoth` in
 * solver/engine.h partition `scanRivalRuns`), so a forking page pays members squared to be ordered. A diff here
 * changes what an ask costs, never what it answers; capping a fork site or shedding an arm would answer a different
 * question.
 *
 * `arrivals - departures == members` is a conservation identity asserted in flow_wfq_census and checkable on the
 * published document; it is exact because each counter has one writer (flow_new appends the only member the
 * registry gains, and flow_remove's swap-remove is the only decrement of `g_flows_n`). Not reset by
 * flow_registry_init: flow_registry_free drains the frontier through flow_release (`while (g_flows_n)`),
 * flow_remove's one caller, so the identity reads 0 == 0 at the next init, which is asserted there and catches a
 * second registry raised over a live frontier. */
static int64_t g_arrivals = 0;
static int64_t g_departures = 0;
/* The departure cause that had no counter. `flow_remove` is the one line a member leaves the frontier on and
   `flow_release` its one caller, so every departure is a flow that completed (engine.c's flow_finish, crediting
   `finished`), one the pager sold (engine_reclaim_tail, `sold`), or the drain in flow_registry_free below, counted
   here, and the three partition `g_departures` (asserted at engine_frontier_census). A member counted here is one
   the frontier still held when its registry went down. Counted at the release rather than from `g_flows_n` before
   the loop, because a release announces a world death to this instance's peers (the end of flow_release), as
   `g_departures` is raised on the line that removes. */
static int64_t g_departures_teardown = 0;
static Flow *g_running = NULL;   /* the flow currently holding the worker (the scheduler sets it) */

/* The fork tree, which the aging term is about, and the one thing in this file that is not a property of a single
 * flow. The term demotes "a monopolizer that burns CPU without emitting", and a fork chain is one monopolizer
 * wearing N names: quickjs.c's step_length_unknown walk forks a "stop at n" arm at every position, each arm runs the
 * rest of the document and finishes, and charging only the arm would leave the walker's family unaged. So a
 * departing flow hands its service to the flow that forked it, hierarchical fair share's rule (a group is charged
 * for everything below it, or any member sheds its debt by splitting); flow_weight is unchanged, only what `cpu`
 * accumulates. A node rather than a `Flow *` parent, because an arm can outlive its parent: the node is refcounted by
 * its owning flow and by every live child's `up`, so the ancestry is addressable exactly as long as something below
 * it can be charged. `owner == NULL` is a flow that left the frontier: a charge walks past it to the nearest live
 * ancestor and compresses the run it walked (union-find's find), so a deep dead ancestry is paid for once. */
typedef struct FlowAcct {
    Flow *owner;              /* the flow this node accounts for; NULL once it has left the frontier */
    /* The node of the flow this one was forked from; NULL for a from-baseline flow. Nothing charges through it and
     * nothing ranks by it: it is read by acct_unref, acct_compress_dead, three preconditions, the fan count in
     * flow_wfq_census, and the one-level branch derivation in flow_fork_inherit. The third accounting scope is
     * `branch` below, one level deep: every flow reaches the top-level arm its subtree hangs under in one
     * indirection, and flow_wfq_census publishes that bucket's live and lifetime membership and lifetime burn.
     * Per-node buckets would be wrong twice: re-keying `branch` to the nearest fork point would move a term of the
     * order (flow_branch_bonus, `1.0 / sub_born` over the bucket, is summed into flow_weight, and flow_fork_inherit's
     * DCHECK fires on it), and it would pin, per departed arm and without bound, the nodes acct_compress_dead exists
     * to free. The minting fan (`br_fan_max`, `br_fan_sum`, `br_fan_depth`), counted by `up` over non-root parents
     * in the census's existing walk, says which deep fork is minting and how wide it stands now.
     * Named residual. Not covered: the burn and lifetime mint count of a deep fork; `sub_us`, `sub_born` and
     * `sub_gone` are maintained only where `branch == self`, and the fan is a gauge that says nothing about
     * receipt. Next diff builds: a per-deep-fork receipt whose maintenance pins no node the compression would
     * otherwise free (a property; the mechanism is not established). Absence shows as: a census in which
     * `br_fan_max` is a large share of `br_fan_sum` while every burn row is a top-level arm's, so which fork minted
     * the frontier is known and what its two sides received is not. */
    struct FlowAcct *up;
    int refcount;             /* the owner's reference + every live child's `up` */
    /* The family's service since its last emission, the quantity flow_weight's aging term is made of, meaningful
       only on a root (`up == NULL`); every flow carries a direct `family` pointer at its root, so it is read and
       written in O(1). It is charged to the family because the reward is the family's (`base + earned` below):
       charging only the arm holding the thread would make each of N live arms burn V / FLOW_AGE_RATE separately
       before it stopped outranking a reward-0 member, N * V / RATE in all, and N is not bounded. So a fork chain,
       one monopolizer wearing N names, is charged as one thing. */
    int64_t fam_us;
    /* The family's accumulated emitted value, the reward term, held at the accounting unit that pays the aging and
     * meaningful only on a root. A credit minted once may be presented once: an emission raising N members' rank
     * against a debit charged to one account would inflate the credit by the bundle's fork factor, and no silence
     * could pay it off. Read through the shared root, the term is rank-neutral across forks at any instants: there
     * is nothing to copy, so two arms of one parent forked at two instants read the same value. Within a family it
     * is a common offset and cancels, so members are ordered by the optimism bonus and their own silence (fewest
     * completed units first, least own burn first), neither reset by the emitting member: the optimism bonus is not
     * moved by an emission (flow_credit_emit), and the own silence is forgiven for every arm of the account at an
     * emission (`emit_gen`) except the emitter, which keeps its burn. Between families it is the whole bandit
     * ordering, charged once against a silence charged once.
     *
     * It is two quantities. `earned` is a ledger, what this account emitted, raised only by flow_credit_emit, one
     * point per finding and unbounded. `base` is a queue coordinate, where the account entered the order: a
     * from-baseline flow founds its own family and is placed at the virtual time of the account in service
     * (flow_arrive_at_virtual_time), a resumed flow at the total the parking session wrote down; neither was
     * emitted. The census's assert that `emit_gen` is past zero exactly when the reward is holds of `earned` and
     * would be false of the sum (an arrived account stands at the leader's reward with `emit_gen` 0). The order
     * reads `base + earned` through acct_family_val, so the split changes no arithmetic; `self_emit` and `val_zero`
     * (flow.h) are the census rows about it. */
    double base;
    double earned;
    /* Whether `base` is a stored tag at all, which makes the coordinate a relation rather than a copy. Zero: the
       account has never held the thread, `base` is not read, and the coordinate is the frontier's virtual time
       (`g_vt`), re-read at every pick. One: dispatched at least once, and `base` is the tag frozen at the virtual
       time it stood at, start-time fair queueing's F taken when the item is first served. The freeze is at
       service, not creation: SFQ's `max(v(t), F_prev)` keeps an unserved member from falling behind because its
       server takes the minimum tag, while this order takes the maximum and an emission raises the served account's
       tag, so v(t) runs away from waiting members, and a coordinate stamped at creation would be passed by
       whatever the incumbent earns afterwards (stranding, for instance, every @S candidate session). A fact about
       the account, never a member: two arms forked at two instants must read it the same, so the arm joins the
       family (flow_fork_inherit) and reads this bit through the same pointer. */
    unsigned char placed;
    /* Which silence window this account is in, bumped by every emission credited here and read only against
       `Flow.cpu_gen`. The aging term's two halves must be measured over one window: `fam_us` is the family's burn
       since any arm emitted, while a flow's own burn since its own emission is a longer window, a debt the
       family's income could not cover; and `Flow.cpu` is written only for the flow holding the thread, so a
       never-dispatched member would carry its parent's burn at the fork for ever, a debt payable only by the
       dispatch it forecloses. So the own half is read over the family's window too: a member whose `cpu_gen` does
       not match this counter has burned nothing since the window opened, and its own silence is zero with no walk.
       uint64 and never wrapped: one increment per finding cannot reach 2^64, and a wrap would alias a stale mark
       onto the live generation. */
    uint64_t emit_gen;
    /* The census's mark, here because a family has no member of its own to be counted through: counting distinct
       families in one walk is a set problem whose identity is this node's address (two families at one `fam_us`
       are two), so marking the root as the walk passes makes the count O(members) with one word per node. Zero is
       unmarked, which `reclaim_calloc` supplies, so a node minted between censuses is counted by the next one; the
       generation skips zero on wrap. Written only by the census. */
    unsigned census_gen;
    /* How many live members of this family stand away from its epoch base: a gauge, meaningful where
       `family == self`, which says whether an index over flow_index_key is a cost or a bar. flow_own_silence is
       `cpu_gen == family->emit_gen ? cpu : 0`, so a member is at base exactly when that reads zero, and
       flow_credit_emit sends a whole family there by moving `emit_gen` with no per-member write; an epoch-keyed
       index rebuilds only the members away. Maintained incrementally at four sites and never walked (a walk at the
       emission would be an instrument changing the run): flow_age_running (a charge takes the running member
       away), flow_fork_inherit (a newborn inherits its parent's silence), acct_depart (an away member leaves) and
       flow_credit_emit (the bump takes the family back and re-stamps the emitter alone). The arrival door is not
       among them: flow_arrive_at_virtual_time asserts `cpu == 0 && cpu_gen == 0`. flow_wfq_census sums this over
       distinct families in the walk that counts members with nonzero flow_own_silence, and the two must agree, so
       a missed transition fires. */
    long away_n;
    /* The top-level arm this node's subtree hangs under: the accounting scope between the member and the family
     * root. A root's is itself; a node forked directly off a root is itself (a new arm); anything deeper inherits
     * its parent's. So every ancestry path holds exactly one node with `branch == self`, set once at creation, and
     * every flow reaches its bucket in one indirection, as `Flow.family` reaches the root, because flow_weight is
     * evaluated inside DCHECK conditions where a compressing walk is forbidden.
     *
     * The order reads it: flow_nonreward sums flow_branch_bonus (`1.0 / sub_born` over this bucket) into
     * flow_weight, FLOW_NONREWARD_MAX carries a line for it, and flow_silence_us_to_sink's bound is `+ 2.0` because
     * of it. It is rank-neutral at a fork for two reasons in two cases: a fork off a non-root moves the newborn into
     * its parent's bucket (flow_fork_inherit), so both read one `sub_born`; a fork off a root gives the arm its own
     * bucket, and the two are equal because both hold one member (nothing raises a root's own `sub_born` past the 1
     * flow_new wrote), the fragile case, asserted where it is decided.
     *
     * It is why acct_compress_dead has a third stop: retention wants the dead spine gone, while a subtree figure
     * wants it (a departed arm's service is part of what its subtree received). Only one node per path must
     * survive its owner, so the compression splices out every dead node between a live member and its branch node
     * and stops at the branch node, pinned by its descendants' `up` references: one extra node per live bucket, and
     * a bucket with no live member is freed like any other. */
    struct FlowAcct *branch;
    /* The thread time every flow in this branch's subtree has ever burned, in microseconds: a lifetime counter,
       meaningful where `branch == self` and never forgiven or reset, unlike `fam_us` (a gauge since the last
       emission), because it answers how much of the thread this side of the branch received. Every microsecond
       charged in flow_age_running lands on exactly one of these, which makes flow_wfq_census's identity exact.
       Microseconds, not notches: the `svc*` rows are thread time / FLOW_SERVICE_US under names that do not say so,
       and a raw total has no quotient to misread. */
    int64_t sub_us;
    /* Every member ever minted into this branch's subtree and every one that has left: lifetime counters whose
       difference is the live membership, a pair because a gauge cannot say whether a bucket of a hundred members has
       minted a hundred or a hundred thousand. Both stay monotone across the one re-parenting: flow_new opens a
       bucket for every flow, and flow_fork_inherit moves the newborn into its parent's as `sub_gone++` here and
       `sub_born++` there, in one statement, never a retraction. */
    long sub_born;
    long sub_gone;
    /* The order's reading of `sub_born`, held once per bucket: `1.0 / sub_born`, the whole of what
       flow_branch_bonus returns and this scope contributes to each live member's weight. Not a cache for speed: it
       makes the term spellable as a bucket quantity, the one form an index can hold per bucket and apply to a whole
       arm (see the residual at flow_index_key). Written on the statement that writes `sub_born` and nowhere else
       (flow_new's initialisation and flow_fork_inherit's join write both adjacently), and acct_branch_offset
       compares it with the quotient recomputed from the integer at every read, so a raise without its offset fires
       at the next weighing. A pure function of `sub_born` at the instant read, monotone downward and published
       nowhere (flow_branch_born publishes the integer); flow_member_key asserts the two routes agree bit for bit. */
    double sub_offset;
    /* How deep in the fork tree this node sits: 0 at a root, parent + 1 at a fork, never moved by the compression
       (which rewrites `up`). A gauge over the members the census reaches that ranks nothing; it says whether the
       fork tree is a star or a chain, which decides whether an aggregate maintained by walking to the root per
       charge (O(depth)) is affordable. */
    int depth;
    /* The census's mark for the bucket walk, separate from `census_gen` because a root is both a family and a
       branch and one field cannot record that it was counted once as each. Same generation value; zero unmarked. */
    unsigned branch_gen;
    /* A third mark with a count under it: how many live members were forked directly off this node (a node can be
       counted as a family, a bucket and a fork parent in one scan). Keyed on `up`, which is exact: `up` has two
       writers, flow_fork_inherit at the fork and acct_compress_dead, which runs only from acct_depart on the
       departing flow's own node, so a live member's `up` is its fork parent's node, written once. A gauge, reset by
       generation on the first member of a scan that reaches its node rather than by a clearing pass. */
    unsigned fan_gen;
    long fan_n;
} FlowAcct;

/* The node census, counted where a node's lifetime begins and ends and asserted at the frontier's teardown, as for
   the four frozen chains. */
static long g_acct_live;

/* Every microsecond this instance has charged, and the share that went to branch buckets whose whole subtree has
   since departed: lifetime counters that make the per-branch burn a conservation. Every charged `us` lands on one
   branch's `sub_us`, and a branch node is freed only when nothing in its subtree is live, its total folded into the
   retired term then, so `sum over live buckets + retired == charged` at every instant, asserted at the census's end
   and checkable on the document. Not reset by flow_registry_init: the drain through flow_release frees every node,
   folding every bucket, before a second registry comes up. Microseconds received, never forgiven, unlike the
   `svc*` rows. */
static int64_t g_charged_us = 0;
static int64_t g_branch_retired_us = 0;

/* Drop a reference: refcount--, free at zero, continue into `up`. A loop, not recursion: the ancestry is as deep as
   the fork tree, which an unknown-length walk makes as deep as the walk, and the C stack cannot be parked. The twin
   of cow_seg_unref / dec_seg_unref / cons_seg_unref. */
static void acct_unref(FlowAcct *a) {
    while (a && --a->refcount <= 0) {
        FlowAcct *up = a->up;
        DCHECK(a->owner == NULL,
               "a fork-tree node was freed while the flow it accounts for is still in the frontier — that "
               "flow holds a reference of its own, so a zero here means the reference was dropped twice and "
               "that flow's next departure charges through freed memory");
        /* A branch bucket's totals outlive the bucket, folded here so the census's burn identity stays exact: a
           node with `branch == self` reaching zero means nothing in its subtree is live. This is also where
           `branch` is shown not to dangle: every live member's `branch` names a node on its own ancestry path,
           which acct_compress_dead keeps, so a bucket freed here has no live member naming it. */
        DCHECK(a->branch != NULL,
               "a fork-tree node reached its free with no branch bucket — flow_new is the only mint and it "
               "points every node at one, so this node was built somewhere else and the thread time charged "
               "through it has been accounted to nobody");
        if (a->branch == a) g_branch_retired_us += a->sub_us;
        free(a);
        g_acct_live--;
        a = up;
    }
}

/* Shorten the run of dead ancestors above `a`, for retention: without this a family whose arms depart one at a time
   retains one node per departed arm while any descendant lives, one per iteration of an unknown-length walk. It
   stops at the root (`t->up` non-NULL), the node every member's `family` tag names, so no compression moves a tag;
   and at a branch node, which a subtree figure needs (FlowAcct's `branch`), for one comparison per trip. The root
   clause is implied by the branch clause (a root's `branch` is itself) and kept because it states the root stop
   directly. It refs the new target before dropping the old link, so the unref cannot free the node it walked to. */
static void acct_compress_dead(FlowAcct *a) {
    FlowAcct *t = a->up;
    while (t && !t->owner && t->up && t->branch != t) t = t->up;
    if (t && t != a->up) { t->refcount++; acct_unref(a->up); a->up = t; }
}

/* A flow leaves the frontier: the node's lifetime and nothing else. No residual service is handed up: `fam_us`
   counts every microsecond the family burns as it burns it, so a hand-up would bill the family twice. What the
   departure owes the tree is the compression above, retention rather than accounting. */
/* The silence reading, forward-declared for its one caller above its definition: acct_depart asks whether the
   departing member was away from its family's base before it clears `f->acct` and `f->family`. In every build,
   since the away count is maintained in every build. */
static int64_t flow_own_silence(const Flow *f);

static void acct_depart(Flow *f) {
    DCHECK(f->acct != NULL && f->acct->owner == f,
           "a flow departed without owning its own fork-tree node — either it never got one, or another flow's "
           "node is standing in for it, and the family this flow's arms are charged to is not its own");
    f->acct->owner = NULL;   /* it has left the frontier: a later compression walks past it */
    /* And an away member leaves the away population (FlowAcct's `away_n`), asked first because the statements
       below clear `f->acct` and then `f->family`, after which flow_own_silence answers zero and the gauge would
       drift up by one per away departure. */
    if (flow_own_silence(f) > 0) f->family->away_n--;
    /* And the member leaves its branch bucket, before the unref below, because the bucket may be this node and this
       is the last instant it is certainly addressable. `sub_gone` rather than a live decrement, for `g_departures`'
       reason. What the bucket received (`sub_us`) is untouched: a departed arm's thread time is part of what its
       subtree got, which is why the bucket outlives it. */
    DCHECK(f->acct->branch != NULL,
           "a departing flow's fork-tree node names no branch bucket — flow_new points every node at one, so "
           "this member's whole subtree is unaccounted and the census's membership identity is about to fail "
           "on a frontier that is otherwise healthy");
    f->acct->branch->sub_gone++;
    acct_compress_dead(f->acct);
    acct_unref(f->acct);
    f->acct = NULL;
    /* The tag goes with it: the root node stays alive while any descendant's `up` chain refs it, so a live flow's
       `family` never dangles, and a departed flow's would once the last of its family goes. */
    f->family = NULL;
}

/* The thread time this flow's whole fork family has burned since its last emission, in one indirection, because
   every flow points straight at its family's root (`family`, joined at flow_fork_inherit). flow_weight calls this
   once per member per pick and is itself evaluated inside DCHECK conditions, so this is read-only: a compressing
   find here would make every assertion in this file mutate the structure it asks about. */
static int64_t acct_family_us(const Flow *f) {
    return f->family ? f->family->fam_us : 0;   /* departed: its node is gone and so is its rank */
}

/* And this flow's own share of that window, the aging term's other half, read over the family's silence window
   (FlowAcct's `emit_gen`): `Flow.cpu` is a reading only while `cpu_gen` matches the account's generation, and past
   an emission the answer is zero, because a flow not charged since the window opened has burned nothing in it.
   This is the one place `Flow.cpu` may be read as a quantity (the weight, both notches, the census rows,
   flow_pick's guard and the arrival rule all come through here), so the field and its generation cannot be
   separated by an edit to one of them; the raw field would charge a member for thread time no emission of its
   family can forgive. Read-only, for acct_family_us's reason; the normalisation happens where the flow is charged
   (flow_age_running), and is observationally a no-op. */
static int64_t flow_own_silence(const Flow *f) {
    if (!f->family) return 0;                   /* departed: its node is gone and so is its rank */
    return f->cpu_gen == f->family->emit_gen ? f->cpu : 0;
}

/* The one relation the aging's two halves owe each other, which makes them summable: a flow's own burn over the
   window is part of its family's burn over that window, so the own half can never exceed the family half. It is a
   claim about the two fields' windows, not their magnitudes, and its inversion is the shape of the defect it
   forbids: an own half measured since the flow's own emission and a family half since any arm's would leave a
   sibling forked before its family emitted at `own > 0 == fam`, paying a debt no emission forgives and no dispatch
   clears (a document whose gate forks a sibling and then an unbounded generator would emit one of its two worlds).
   A macro expanded at every transition that can move either half, so the abort names the transition; not in
   flow_own_silence, which is called once per member per pick and inside other DCHECK conditions, so it would name
   one line for every caller. Both sides are read-only, so it is a legal DCHECK condition. */
#define DCHECK_AGING_ONE_WINDOW(f, what)                                                                     \
    DCHECK(flow_own_silence(f) <= acct_family_us(f),                                                          \
           "a flow's OWN silence exceeds its FAMILY's over the same window, after " what " — the aging term "  \
           "SUMS the two and weighs them against the family's single reward, so this says they are being "     \
           "read over two different epochs and the sum is a unit error. The arm now carries a debt its "       \
           "family's next emission cannot forgive, repayable only by the dispatch the deficit itself "         \
           "forecloses, which is §scheduler's razor's STARVES: fix the transition this abort names so both "   \
           "halves move over the epoch FlowAcct's `emit_gen` keeps")

/* The queue coordinate, forward-declared because the frontier's virtual time below is a reading of it and the
   ordering is stated once, further down beside the terms it is made of. */
static double flow_queue_weight(const Flow *f);

/* And the reward alone, forward-declared because the clock's writer samples that and not the queue weight (see
   frontier_vt_serve for why the server's own aging may not enter the coordinate a newcomer arrives at). */
static double acct_family_val(const Flow *f);

/* And the one statement that says the order moved, forward-declared because the clock's writer below raises it and
   the raise is defined with the registry it belongs to. */
static void frontier_rank_changed(void);

#if APICLIENT_DEV
/* And the member half of the weight, forward-declared for the one writer that moves it without raising the
   statement above: flow_age_running is deliberately silent (its banner: the charge is what the aging term reads,
   and a bump per charge would invalidate every cached rival every dispatch), so it maintains the stamp flow_best's
   walk compares against. Defined beside the terms it is made of. Dev-only in declaration and use, for
   `Flow.key_last`'s reason. */
static double flow_member_key(const Flow *f);

/* And the per-member half of that key, for the same writer and a different claim: flow_member_key is the quantity
   that must stand still between two frontier generations, while this is the quantity an index may be keyed on
   (flow_index_key's banner: one quantity, two questions, and the stricter decides the composition). The stamp
   above asserts the sum and this one the half, at the same seam, for solver/flow.h's `ikey_last` reason. */
static double flow_index_key(const Flow *f);
#endif

/* The frontier's virtual time: SFQ's v(t), the service tag of the item in service, stored as a scalar because it
 * must be readable when nothing is in service (the frontier persists across slices and sessions, so a definition
 * that answers only while `g_running` is set would place every newcomer differently depending on whether the host
 * was between two dispatches).
 *
 * It is written at one event, an emission by the item in service, which raises that item's coordinate by what it
 * earned, so v(t) follows it up; between emissions the clock is constant, as SFQ says, so a newcomer's placement is
 * a fact rather than a race with its creation microsecond. A dispatch does not write it: moving the clock to the
 * winner's own coordinate would lift every unplaced account the pick compared it against to that coordinate,
 * where a full optimism bonus puts them above it, and the pick would un-choose its own winner (see
 * flow_set_running). The clock follows what the frontier earns, never which member holds the thread.
 *
 * Every write raises the frontier generation at the writer: the clock is a term of every unplaced account's
 * weight, and the preempt hook's rival cache is correct only if such a move arrives through the generation.
 *
 * It does not follow the aging down: the item in service sinks as it burns, and following it would charge every
 * never-served member for the thread time of the one being served. A member given nothing owes nothing.
 *
 * Floored at zero, the clock's origin rather than a clamp: the first flow enters at v(0) = 0 and none enters
 * below, so a serving tag below zero (an item that has burned more than its whole reward, flow_pick's monopolizer
 * state) moves the clock no further back than the busy period's start. The floor also keeps that guard's
 * derivation true: it reads `val + 1` off the picked flow and an untouched member, and a negative reward on a
 * member that did nothing would fire it on a healthy run.
 *
 * Read-only and O(1) at every reader, because acct_family_val is called once per member per pick and inside DCHECK
 * conditions. */
static double g_vt;

static double frontier_vt(void) {
    DCHECK(g_vt >= 0.0 && g_vt == g_vt,
           "the frontier's virtual time is not a coordinate any member could have entered at — the clock is "
           "floored at the start of the busy period where it is written, so a negative or NaN reading here is "
           "a second writer that did not go through frontier_vt_serve, and every unplaced account is being "
           "ranked at a position no flow has ever stood at");
    return g_vt;
}

/* The one writer of it. `f` is the item whose tag the clock is now a reading of, the flow in service whose own tag
   an emission (or a restore) has just raised, already installed as `g_running`. */
/* Does the clock still lead every account: the definition of `g_vt` asked as a question, for the assert in the
   writer below. Walked over the members, as the census counts families; a family reached twice answers the same
   value, so no marking is needed for a maximum. Mentioned only inside a DCHECK condition, so release neither calls
   nor emits it, as flow_is_min_weight. */
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
    /* The reward, not the queue weight: the queue weight carries the server's own aging, grown by the whole
       family's thread time, so a coordinate sampled from it would charge every newcomer for the leader's debt, and a
       member given nothing owes nothing. The coordinate a newcomer arrives at is what the frontier has earned. */
    v = acct_family_val(f);
    v = v > 0.0 ? v : 0.0;      /* the clock's origin: no member has ever entered below the busy period's start */
    /* And it is the maximum over accounts, taken incrementally: SFQ's `max(v(t), F)` computed here rather than at
       every arrival. Not a clamp over a falling quantity: `acct_family_val` is `base + earned`, with `base` frozen
       at placement and `earned` a ledger nothing decreases, so no account's coordinate ever falls, and a lower
       sample is a lesser account emitting; the maximum answers where the front of the order stands. The
       incremental form is exact, because every increase of an account's `acct_family_val` passes through one of
       three sites, all covered: an emission (`earned += v`, which calls this); a placement (flow_set_running writes
       `base = frontier_vt()` and asserts `earned == 0`, so the value equals `g_vt`); and a cold-tier restore
       (flow_restore_reward writes a parked coordinate that can lead, and calls this). An unplaced account stands
       exactly at `g_vt` (acct_family_val asserts `earned == 0` there). The assertion below checks there is no
       fourth writer. */
    if (v > g_vt) g_vt = v;
    /* The definition, asserted: `g_vt` is the maximum coordinate any account stands at. Stronger than
       monotonicity, since a clock that never decreases can still sit below the leading account, so newcomers would
       arrive underneath a member they are entitled to tie, silently, for the rest of the session. It catches a
       fourth writer of a reward. O(members) and dev-only, as flow_is_min_weight, and paid once per emission rather
       than per pick. */
    DCHECK(acct_vt_leads(), "an account stands at a reward ABOVE the frontier's clock — the clock is the "
           "coordinate every newcomer and every unplaced account is ranked at, so a leader the clock does not "
           "know about means arrivals are entering BELOW a member they are entitled to tie, permanently and "
           "with no row saying so. A reward was written by a site that did not serve the clock");
    /* And moving the clock is a rank change, raised at the writer: this value is a term of every unplaced account's
       weight, and the preempt hook's rival cache is correct only if every such move arrives through the frontier
       generation (engine.c: "the eligible set the hook ranks against is the same one the pick used"). Raised on
       every call, not only where the maximum moved, since a caller here has just written a reward and an account's
       own coordinate changing re-ranks that account. */
    frontier_rank_changed();
}

/* And the reward that silence is subtracted from, read through the same pointer (FlowAcct's reward): the WFQ's
   reward term and the only reading of it the order makes (`Flow.val` is what one member emitted, a census quantity).
   Exported as flow_reward (flow.h) because the node is private to this file and the ranked-state cache and the cold
   tier both name the quantity the weight is a function of. */
static double acct_family_val(const Flow *f) {
    /* The order's reward is the sum of the two, spelled only here, so the split cannot leak into a caller that adds
       one of them. `base` is where the account entered the order and `earned` what it has emitted since. While the
       account has never been served the first is a relation: an unplaced account has emitted nothing (only the
       flow holding the thread is credited, and holding it places the account), so its reward is the frontier's
       virtual time re-read at every pick, SFQ's `max(v(t), F)` as a continuing relation; once served, its tag
       freezes and it stands on its own ledger. The `+ earned` is written on both arms rather than factored out:
       it is zero on the unplaced arm by construction (asserted), and factoring it would read as an arm that
       discards a ledger rather than one where it is empty. */
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

/* A rebuilt flow's account, replaced rather than inherited: the one writer of the reward that is neither an
   emission nor an arrival, called only by the cold tier's rebuild. flow_add places a from-baseline flow at the
   frontier's virtual time so a newcomer enters beside the flow in service, but a rebuilt flow is a member coming
   back with the account the parking session wrote down: adding would credit this session's findings to last
   session's flow, and keeping the placement would discard the cross-session ordering the park exists to keep ("a
   high-value flow suspended last week resumes ahead of a low-value fresh one today"). It may only write a family of
   its own: a rebuilt flow founds its own family (flow_add_unseeded mints it a root and joins it to nothing), and
   writing through a joined tag would re-place every arm of another family. Asserted, since the caller cannot see
   the tag. */
void flow_restore_reward(Flow *f, double val) {
    DCHECK(f != NULL && f->acct != NULL && f->family == f->acct && f->acct->up == NULL,
           "a rebuilt flow's parked reward was written through a fork family it does not own — a resume founds "
           "its own account, so a joined tag here re-places every live arm of another family at a reward that "
           "belongs to one parked recipe");
    DCHECK(val == val && val >= 0.0 && val < 1e300,
           "a parked reward came back as something the WFQ cannot order by — a NaN compares false in both "
           "directions, so the resumed frontier's order would fall back on registry position");
    /* A resume restores a coordinate, not earnings: the parked number is where this recipe stood when its session
       ended, and crediting it as `earned` would report last session's findings on the `self_emit` row and break the
       census's forgiveness identity; `base + earned` is unchanged. And it is a tag, placed by the statement that
       writes it: `base` is read only once the account is placed, so writing the parked number without placing it
       would rank the recipe at this session's clock.
       Named residual. Not covered: a recipe parked before it was ever served comes back placed and should come back
       reading: cold.c parks a never-served account as `%.17g` of flow_reward (this session's clock reading), and it
       is restored here as a frozen tag, so it re-enters at a foreign clock rather than tracking the one it now
       belongs to. Correct for a member coming back that has earned. Next diff builds: a park record carrying
       `base`, `earned` and the placement bit apart (cold.c's format and writer), so a rebuild restores a served
       account at its tag and a never-served one unplaced. Absence shows as: a resumed session reporting
       `valUnplaced: 0` at its first census while rebuilt recipes stand, with `valMin` pinned at a coordinate no
       member of this session was served at. */
    f->family->base   = val;
    f->family->placed = 1;
    /* And the clock learns about it, the third writer of a reward: a restored leader the clock did not know about
       would leave arrivals entering below a member they are entitled to tie for the rest of the session. It is one
       of the three sites frontier_vt_serve's incremental maximum enumerates, and the serve also raises the
       frontier generation, which a rebuild owes, since a member coming back at its own tag changes the order. */
    frontier_vt_serve(f);
}

/* The ranking changed, so the running flow's claim on the thread may have: the value yield fires "the moment a
   parked flow outranks (or on an emit/fork/suspension that changes ranks)", and this is that moment (a sibling
   landed, a flow left, the running flow emitted). Bumping the generation alone would make the answer available but
   unasked until the running flow reached a suspend point, so the bump raises the engine's yield request: the running
   flow reaches its next opcode boundary, the poll asks preempt_hook, and the WFQ decides there. Raising is not
   deciding: a flow that still outranks everything keeps the thread for one declined hook call. */
static void frontier_rank_changed(void) {
    g_gen++;
    /* And the same event counted for the life of the instance: flow_registry_init resets `g_gen` and not the scan
       counters, so the generation number and `scanRivalRuns` would be over two lifetimes once a host initialised a
       second registry, and a ratio of them would be about no run. Counted here, the one place a rank change
       happens. */
    g_rank_changes++;
    JS_RequestFlowYield();
}

/* The frontier's members, in registry order. A walk, not a rank: the pending-fetch register lives on the flows, so
   whoever asks what the host owes visits all of them. flow_best answers a different question. */
Flow *flow_at(int i) { return (i >= 0 && i < g_flows_n) ? g_flows[i] : NULL; }

void flow_registry_init(const char *doc_name) {
    /* The frontier this replaces must have gone. The assignment below drops `g_flows`, so a registry raised over a
       live frontier leaks every standing flow (its COW delta, DOM head, suspended frame and queued jobs), and no
       per-member invariant can see it because no member is left to ask. `g_arrivals - g_departures` counts the
       members this instance still holds; it is checked against `g_flows_n` rather than zero so this is the
       identity the census publishes, not a second claim. */
    DCHECK(g_arrivals - g_departures == (int64_t)g_flows_n,
           "the flow registry is coming up over a frontier that never departed — every member still standing "
           "is about to become unreachable with its delta, its DOM head and its suspended frame, and the "
           "arrival/departure pair is the only thing that can see it because there will be no member left to "
           "ask. flow_registry_free drains through flow_release, so this is a host that raised a second "
           "registry without taking the first one down");
    g_flows = NULL; g_flows_n = 0; g_flows_cap = 0; g_gen = 0; g_running = NULL;
    /* The queue's clock comes up with the queue, at SFQ's v(0) = 0. Left standing, it would place this frontier's
       first from-baseline flows at the coordinate another document's leader reached: a lifetime quantity read as
       an instant. */
    g_vt = 0.0;
    /* The world namespace comes up with the frontier: every flow added below is minted a world named by this
       document, and an unnamed world could not be reached from another instance. */
    world_registry_init(doc_name);
}

unsigned flow_frontier_gen(void) { return g_gen; }

/* How many times the order changed, for the life of this instance: the denominator the preempt hook's rescan
   count has and `scanNextRuns` is not. See solver/flow.h. */
long flow_rank_changes(void) { return g_rank_changes; }

/* How many members this instance has admitted and how many it has let go: whether the frontier is drained at all,
   the precondition for the order deciding anything. `arrivals / picksLifetime` is members minted per dispatch; see
   the banner at `g_arrivals`. */
int64_t flow_arrivals(void) { return g_arrivals; }
int64_t flow_departures(void) { return g_departures; }

/* And how many of those departures were the instance going down under them: the third arm of the departure total,
   exported so engine.c's engine_frontier_census, which holds the other two, asserts that the three sum. See the
   banner at g_departures_teardown. */
int64_t flow_departures_teardown(void) { return g_departures_teardown; }

/* How many times the scheduler's pick returned an already-dispatched member while a never-dispatched one stood at
   exactly the same weight: starvation, counted at the one line that decides which flow runs, which no census gauge
   can read. See solver/flow.h. */
static long g_starved_picks = 0;
long flow_starved_picks(void) { return g_starved_picks; }

/* The whole rebuild an epoch-keyed index would pay, summed over a run: the reading that decides whether a
 * sub-linear order over flow_index_key is buildable. At every emission the members standing away from their
 * family's base are the ones such an index must move, so this adds `away_n` at each bump.
 *
 * A lifetime sum, not a gauge: a census lands at an arbitrary point between two emissions, so a gauge of the away
 * population reads near zero just after one and at its peak just before. The gauge is also published, for the
 * identity, and is read as a shape, never as the cost.
 *
 * Its denominator is `scanNextWeights`, the lifetime sum over scans of the members each one weighed, which is the
 * walk an index would replace: well below it the index pays; at or above it the rebuild is the walk moved rather
 * than removed. `epochResetsLifetime` is the number of bumps, so the quotient is the mean rebuild per emission.
 *
 * Release-live, unlike the key and index stamps: the maintenance is four O(1) statements, so the row can be read
 * off the release artifact, which is where the product runs. */
static long g_epoch_rebuild = 0;
long flow_epoch_rebuild(void) { return g_epoch_rebuild; }

/* And how many times the epoch moved: the counter above's denominator, a lifetime count of emissions that reset a
   family's base. Raised in the same statement group as the sum and nowhere else, so the two describe one
   population. */
static long g_epoch_resets = 0;
long flow_epoch_resets(void) { return g_epoch_resets; }

/* And the subset of those picks in which the re-dispatched member had nothing to continue. A member re-picked
   while inside a unit of work is being handed the thread to finish a program, which is necessary work and not
   starvation, so `g_starved_picks` sums two populations that call for opposite repairs, and this row separates
   them. See solver/flow.h for the reading and flow_between_units for the boundary. */
static long g_starved_picks_idle = 0;
long flow_starved_picks_idle(void) { return g_starved_picks_idle; }

/* How deep the plateau is: the complement of `g_starved_picks` at the same line. `g_starved_picks` counts the pick
 * that displaces the incumbent while a never-dispatched member stands level; these count the pick that keeps it.
 * The value yield blesses that population ("a top-ranked flow runs on at ~zero switch cost") while the optimism
 * bonus must end it, so these rows tell the two requirements apart.
 *
 * They are not a starvation count. A single retention is the value yield working; the claim is a bound on how long
 * it lasts (flow.h's aging term: "a flow tied with an unrun sibling on reward and bonus hands over after ONE
 * quantum"), a claim about the depth of a stretch, which is why the run count below turns the count into a depth.
 * The demotion that ends a stretch is certain: flow_age_running is called unconditionally by the dispatch loop and
 * its charges telescope across the quantum (engine.c carries the previous turn's reading into the next `t0`, and
 * asserts that observability at the charge), so an incumbent's notch outruns a level waiter's by about two per
 * quantum. What nothing states is how many scans fit inside that quantum, and that is the plateau's depth.
 *
 * Release-live, for `g_epoch_rebuild`'s reason: four O(1) statements over quantities the scan already computed.
 * Lifetime counters off the flows, for `g_picks_total`'s reason, and reset by nothing, for `g_rank_changes`', so
 * they difference across two samples of one instance. See solver/flow.h for the two quotients; flow_between_units
 * for the boundary the idle subset is an upper bound on. */
static long g_plateau_asked = 0;
long flow_plateau_asked(void) { return g_plateau_asked; }

/* And the subset of `g_plateau_asked` in which the incumbent kept the thread: contained in it by construction,
   being raised inside its `if`, so the pair is the share of makeable comparisons the tie-break gave the incumbent. */
static long g_plateau_held = 0;
long flow_plateau_held(void) { return g_plateau_held; }

/* And how many maximal stretches those retentions formed: the denominator that turns the count into a depth.
   `held / runs` is the mean number of consecutive scans an incumbent kept the thread while an untouched member stood
   level with it:
     runs == 0, ask > 0              the tie-break never favoured the incumbent while a never-run member was level;
                                     the guarantee is not under test on this run.
     held / runs bounded             the queue rotates: every stretch ends, and the depth is what one quantum
                                     costs in scans.
     runs == 1, held large           one unbroken hold for the whole run: the guarantee is false, and this is the
                                     one reading here that asks for a diff.
   A lifetime count, not a high-water mark: a maximum saturates and its plateau looks like a ceiling, while two sums
   over one walk give a mean that differences into a rate over a window. A stretch is closed by any dispatch scan
   that did not hold, including one that was not asked (nobody untouched is level, so nobody is passed over).
   `g_plateau_open` is that boundary and the one piece of state here; it decides nothing and no term or pick reads
   it. Reset by nothing, as the counters. */
static long g_plateau_runs = 0;
long flow_plateau_runs(void) { return g_plateau_runs; }
static int g_plateau_open = 0;

/* And the subset of retentions in which the incumbent had nothing to continue: the half of `held` the value yield
   does not bless, since a member at the unit boundary has finished its trial.
   Named residual. Not covered: two of the three clauses of engine.c's unit boundary, so this is an upper bound on
   "the incumbent had nothing to continue". The park clause is unaskable here: a parked continuation is a
   per-member fact for every member except the one holding the thread, whose queue lives in the runtime for its turn
   (flow_holds_park asserts it is not asked), and this row is about the running member. The microtask clause is
   askable but not asked, since flow_job_microtask walks the job queue taking and freeing a JSValue per entry and
   this row fires on nearly every scan of a tied frontier. Next diff builds: the runtime handle at the pick (engine.c
   asks JS_HasParkedFlow; this file holds no handle), with an O(1) microtask predicate, a count carried on the flow;
   either alone leaves the bound. Absence shows as: this row at or near `plateauHeld` on a document with a large
   `jobsQueued`, where only `jobsQueued`/`jobsRun` beside it separates holding queued continuations from idling. */
static long g_plateau_held_idle = 0;
long flow_plateau_held_idle(void) { return g_plateau_held_idle; }

/* A flow takes or releases the thread, and a dispatch is where an account's coordinate stops being a reading of
   the frontier's clock and becomes a tag. The two statements are one operation and in this order, because between
   them the clock would be defined by an account defined by the clock.
   The stamp is value-preserving by construction, and asserted: an unplaced account's reward is frontier_vt(), so
   freezing that reading into `base` cannot move the flow's coordinate by an ulp. The assert catches a stamp that
   writes some other quantity (the flow's own weight, the incumbent's reward, a per-member sum), which would move a
   member at the instant it is picked.
   Releasing the thread leaves the clock where it is: v(t) holds between busy periods, so a newcomer created between
   two slices enters where the frontier last stood rather than at zero. */
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
    /* The clock is not moved here. Serving the clock at a dispatch sets it to the winner's own queue coordinate,
       so an unplaced never-run account R stands at `g_vt + 1.0 + distance(R)` while the flow just dispatched stands
       at `g_vt + 1/(1+visits) + distance`: R outranks it strictly once it has completed one unit, with no term of
       the winner having moved. The pick's own choice would promote the tail above it, after the pick compared them
       and before engine.c records what the winner was ranked on. The argument for following the item in service
       is about findings, and the emission caller of frontier_vt_serve answers it. Because every writer of the
       clock raises the frontier generation, the pick and the preempt hook read one clock, and a rival cached at a
       generation is one the pick was shown. Placement above is rank-neutral and raises nothing: freezing `base`
       makes the account stand on the identical value, which the DCHECK asserts. */
}
Flow *flow_running(void) { return g_running; }

/* How many times a finding was offered to the order, and how many of those the order could not be given: the
 * partition `valTop`, `topForgiven` and `selfEmit` presuppose. All three read zero both when no detector fired (a
 * reach question: build the surface) and when detectors fired with no flow to pay (a question about which side of
 * the host/flow boundary the detection happened on), and only these rows separate the two.
 *
 * The drop is correct, which is why it is counted rather than fixed. This function is reached on host time:
 * core/loader/document_load.c's "the production ABI's `qjs_init`, which parses before the first flow is seeded",
 * so every subresource the root document's markup names is minted by endpoint_record with `g_running` NULL. No
 * flow discovered it, so there is no account to credit; a plain parse gives the `<head>`, and paying an arm for it
 * would pay for work it did not do. A credit made at the wrong site looks identical from outside the process,
 * which is the argument for the rows.
 *
 * Three counters rather than two, for `g_prog_starts`' reason: the total is raised at the one entry and each arm on
 * the line that decides it, so a third exit added later breaks the identity flow_wfq_census reads. Lifetime
 * counters (flow.h states the kind): none falls and nothing resets them, so they difference across two censuses.
 * A report, never a bound: no term, pick, cap or shed reads them. The record retires when a credit made outside a
 * slice has an account to land on, since the dropped arm is then unreachable. */
static int64_t g_credit_calls = 0, g_credit_paid = 0, g_credit_dropped = 0;

/* Credit the running flow with newly emitted value (a new @H endpoint or @S PoC): its family's reward rises and its
   aging window resets, so a productive flow outranks fresh and starved flows. Called by the detectors (endpoint.c,
   solve.c) when they record something new; this is what orders the WFQ by value of information rather than
   breadth-first by visit count. The rank change is raised through frontier_vt_serve. */
void flow_credit_emit(double v) {
    int64_t own_us;   /* the emitter's own burn over the window this emission closes — see the bump below */
    /* One emission is one point, the unit the aging term's exchange rate is stated in. A credit of zero or less
       would reset a flow's aging without changing its rank, the one way a monopolizer could refresh its lead
       without producing anything, so the sign is asserted where the reward enters. */
    DCHECK(v > 0.0, "a flow was credited a non-positive emission — the WFQ's reward term counts NEW @H/@S "
                    "findings at one point each, and a zero credit resets the aging that outranks a monopolizer "
                    "while adding nothing to weigh it against");
    /* The offer, counted before the one test that can refuse it, so the two arms below partition the total. */
    g_credit_calls++;
    /* And the arm with nobody to pay. Not an error, so no assert: a detector reached on host time (the root
       document's markup, inventoried by `qjs_init`'s parse before `qjs_begin` seeds the frontier) has no running
       flow because no flow discovered it. Counted, for the banner's reason. */
    if (!g_running) { g_credit_dropped++; return; }
    /* The order's reward is the family's, so the credit lands on the family (FlowAcct's `val`): raised once, the
       silence charged to that family cancels exactly it, while raising every arm would present one emission as
       many times as the bundle branches against a debit charged once. A running flow is a member and owns its
       account, so a NULL family here is a flow credited after it left. */
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
    g_credit_paid++;   /* and the arm that reached an account: the ledger write and its count are one event */
    /* And the frontier's clock, because the item in service is what the clock reads and its tag has just moved.
       This is frontier_vt_serve's one event, and it makes v(t) a continuing relation: without it every account
       never served would stand at the leader's coordinate when last picked, and be passed by the findings the
       leader makes during the slice it holds. */
    frontier_vt_serve(g_running);
    /* And this member's own ledger, which ranks nothing. The family total is what an account has emitted and is
       the order; this is what this flow emitted, which the census asks (a frontier retiring members while every
       one reads zero here is coasting on an ancestor's findings). Never copied, inherited or read by flow_weight. */
    g_running->val += v;
    /* An emission does not touch the optimism term's `visits`. The reward is the family's, so within a family it
       is a common offset, and a zero written here would be copied by every later fork: an arm born after an
       emission would carry a full 1.0 while its sibling born before carried `1/(1+V)`, presenting one credit
       twice (to the family through `val`, and to the member holding the thread). A within-family preference for
       the productive member is what `Flow.val` would express, and it is deliberately unranked. A family's members
       are ordered by fewest completed units and least own silence; a flow that emits keeps the thread until it
       finishes its unit (flow_pick's comparison is strict), its count advances, and it hands over on its own
       visit. `visits` has one meaning: units completed on this flow's prefix, raised only by flow_credit_visit and
       carried by a fork. The rank still changes on an emission through the reward, both silence halves and the
       generation bump. */
    /* And the aging, both halves, in one statement about the account. The two halves are summed into one notch and
       weighed against one reward, the family's, so a debt measured over a window the family's income does not
       cover is a unit error. `Flow.cpu` is written only for the flow holding the thread, so a separate per-flow
       reset would leave a never-dispatched member carrying its parent's fork-instant burn for ever. The generation
       bump makes the own half read zero for every member of the family at once, in O(1); `fam_us` is the family
       half of the same window. It does not flatten the order: once the thread is handed to one member, that member
       is charged (flow_age_running) and sinks at FLOW_AGE_QUANTUM per quantum against its siblings, a comparison
       the waiting side wins by waiting.
       The emitter's own burn over the closing window is read through flow_own_silence before the bump: the raw
       field is that quantity only while its mark is current, and a flow that emits twice with no charge between
       holds a previous window's arithmetic in it. */
    own_us = flow_own_silence(g_running);
    /* The rebuild this bump costs an index, taken before it happens (see `g_epoch_rebuild`): after the bump moves
       the generation there is nothing left to count. Raised with its own denominator. */
    g_epoch_rebuild += g_running->family->away_n;
    g_epoch_resets++;
    g_running->family->emit_gen++;
    /* And the gauge goes with the window it counted: every member now reads zero own silence, so the away
       population is empty for an instant; the emitter is put back below. */
    g_running->family->away_n = 0;
    /* And the emitter carries its own burn across the bump. The bump forgives the own half for every arm, which is
       right for every arm except the one holding the thread: the credit is the family's (`earned`, read by every
       arm) while the debit erased would be the member's (`cpu`), one credit with two beneficiaries, the argument
       that keeps `visits` untouched above. Once the reward and the family half cancel as common offsets, the own
       half is the only within-family ordering signal. Without it, with flow_credit_visit unable to fire inside a
       program (it asserts `frame == NULL`, and a fork copies `visits`), every member of a one-family frontier
       reads one weight, the strict pick keeps the thread where it is, and the never-dispatched tail stands at the
       front for ever. So `fam_us` becomes the emitter's burn and nothing else, and the aging relation holds with
       equality at this transition.
       Named residual. Not covered: an arm that burned the thread earlier in this window and did not emit still has
       its own half forgiven here. Fair queueing over the arms would remember every arm's service; this is a
       one-emission memory, correct for the emitter, which must hand over next. Next diff builds: the own half kept
       per arm across the bump, a per-member window mark the O(1) bump cannot reach, so a change to how `emit_gen`
       forgives. Absence shows as: `picksMax` staying at the mean sweep depth between two emissions (flow.h's
       `top_forgiven` reading) while `neverPicked` climbs with `members`, a sweep that restarts.
       It narrows flow_pick's `unrun` population, which needs every non-reward term at zero: `fam_us` is non-zero
       right after an emission, so the two ordering guards there short-circuit within one quantum of a finding, but
       only when the emitter burned a whole cooperative quantum between two findings (below FLOW_SERVICE_US the
       notch floors to zero). */
    g_running->cpu             = own_us;
    g_running->cpu_gen         = g_running->family->emit_gen;
    /* And the emitter is the whole away population again: it is re-stamped into the new window carrying `own_us`,
       so if it burned anything it stands away from a base every sibling is at. Tested on `own_us`, the value just
       stored, rather than by reading flow_own_silence back. */
    if (own_us > 0) g_running->family->away_n = 1;
    g_running->family->fam_us  = own_us;
    /* The two halves are equal for the emitter and zero for every other arm: the family's burn over the new window
       is the emitter's, the only burn that survived the bump. The mirror transition (the family half zeroed while
       a sibling's own half holds a previous window's burn) is caught because the macro is expanded at every
       transition that can move either half. */
    DCHECK_AGING_ONE_WINDOW(g_running, "an emission forgave this family's window");
    /* The rank change is raised by frontier_vt_serve above and not again here: an emission is one event and
       `g_rank_changes` is a lifetime count of order changes, so a second raise would double-count every rate built
       on it, the preempt hook's rescan cadence first. An emission reaches the clock's writer unconditionally past
       the early return. The yield request is a bit polled at the next opcode, so raising it before `emit_gen` and
       `fam_us` are updated is observationally identical. */
}

/* The other kind of quantity in the weight. flow_credit_emit is a ledger entry: it records that something was
 * learned, so it is paid once and adds. This is a comparator reading: it records where a candidate stands, so it
 * is written, not added, and says the same thing however many times it is taken. A distance paid into a ledger
 * would have to be paid at most once per search, which makes every candidate after the first indistinguishable
 * from one that never started.
 *
 * Monotone, as an invariant rather than a clamp: a flow's payload is fixed for its life, so the longest run of it
 * any re-execution delivered can only be discovered, never undone. A later lower reading is another sample of the
 * same fixed question (taking it would let a page that writes the payload twice demote the flow for its second
 * write), so it is discarded, and the early return keeps the generation still for a rank that did not move.
 * Keyed on the flow passed, not the running flow: the observation is about a specific flow's bytes the caller
 * holds, so this is not a second way to ask which flow is running. */
void flow_observe_replay(Flow *f, long consumed, long total) {
    double frac;

    DCHECK(f != NULL,
           "an @S candidate's runway position was recorded against no flow — the fraction is a reading of ONE "
           "flow's progress along ITS OWN recorded path, so a caller with no flow measured somebody else's");
    DCHECK(f->cand_payload != NULL,
           "a runway position was recorded for a flow carrying no payload — the runway is the approach to a "
           "candidate's own source read, so a flow with no payload has no source read to be approaching and "
           "would rank above every exploration flow on a measurement of nothing. The population is the caller's "
           "to state, not this function's to sniff: decide.c asks the question only of a candidate");
    DCHECK(total > 0 && consumed >= 0 && consumed <= total,
           "an @S candidate's runway position is not a position on its own path — `consumed` is the arms this "
           "run has REPLAYED and `total` the recorded path they are a prefix of, so a value outside [0,total] "
           "is a numerator taken from one flow and a denominator from another, and the fraction it makes enters "
           "the weight beside an optimism term whose entire range is 1.0");
    /* Past the delivery this rung is pinned and may not be written, the ladder's own rule: the source read is
       behind this flow, and a live fraction would rank a delivered candidate whose recorded path runs on past its
       source read below an undelivered one that consumed all of its own. Asserted rather than skipped, because
       reaching here in that state means dec_replay still consumes arms for a flow whose delivery is recorded. */
    if (f->cand_rung >= FLOW_RUNG_DELIVERED) {
        DCHECK(f->cand_replay == 1.0,
               "a delivered @S candidate's runway rung is not pinned — flow_observe_rung pins it at the "
               "delivery because past the source read the runway is not the question, so an unpinned value "
               "here is that pin having been missed and the ladder's bottom two bands now overlap");
        return;
    }
    /* The triple describes one sample, asserted on the way in. Below the delivery the fraction is exactly the
       division of the pair beside it, so a disagreement is two writers reaching these fields separately and the
       report built on the pair would contradict the comparator. Asserted after the pin's early return, because
       past the delivery the fraction is a policy and the pair the last observation, which may differ (see the
       field). */
    DCHECK((f->cand_replay_of == 0) == (f->cand_replay == 0.0),
           "an @S candidate's runway fraction and the pair it was computed from disagree about whether any "
           "reading has ever been taken — `of == 0` is this flow's word for 'never observed' and so is a zero "
           "fraction, so one of the two was written without the other and the report would state that a replay "
           "consumed nothing about a flow the comparator is already ranking as having moved");
    DCHECK(f->cand_replay_of == 0 ||
           f->cand_replay == (double)f->cand_replay_arms / (double)f->cand_replay_of,
           "an @S candidate's runway fraction is not the division of the pair stored beside it — the three are "
           "written together at one sample by the three assignments that end this function, and by nothing "
           "else, so a mismatch means the report's two "
           "halves and the comparator's one number are describing different moments, which is the exact "
           "defect holding a fraction as its halves exists to remove");
    frac = (double)consumed / (double)total;
    /* Monotone, as flow_observe_survival one rung down: a candidate's recorded path is fixed for its life, so the
       longest prefix any replay consumed can only be discovered, and a lower reading would demote a flow for a
       re-execution that diverged earlier. The early return keeps the frontier from re-ranking. */
    if (frac <= f->cand_replay) return;
    /* All three in one place, because the pair is the fraction and not a second observation of it; the asserts
       above enforce that on every later call. */
    f->cand_replay = frac;
    f->cand_replay_arms = consumed;
    f->cand_replay_of = total;
    frontier_rank_changed();
}

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
    /* And the rung below this one, the ladder's order asserted from its own side. A payload says the substitution
       was installed, not that it happened, and the run about to be recorded is a run of the payload's bytes in a
       string, which the page's own text matches just as readily because a breakout is punctuation. solve.c's three
       candidate-arm entries return before here unless the substituting component reports it performed; this is the
       same statement one layer down, so a fourth sink class or route cannot forget to ask. It also makes
       flow_distance's `cand_surv + cand_rung` sound: the fraction is arithmetically below the delivery and
       chronologically above it, and this asserts that a nonzero fraction at rung 0 cannot exist. */
    DCHECK(f->cand_rung >= FLOW_RUNG_DELIVERED,
           "an @S survival fraction is being recorded for a flow that has not been observed to DELIVER its "
           "payload — the run about to be measured is the page's own bytes coinciding with the candidate's, "
           "so the flow's fitness, the search's ratchet and the report's surviving-byte count would every one "
           "of them be a reading of text the attacker never supplied");
    if (frac <= f->cand_surv) return;  /* not an improvement: the rank did not move, so nothing may re-rank */
    f->cand_surv = frac;
    frontier_rank_changed();
}

/* The same quantity one rung up; flow.h says why the writers are separate entry points, why the ladder's bottom rung
 * sits below every stage @S names, and why the fire is not on it. Everything flow_observe_survival says about a
 * reading rather than a payment holds here; this adds the order, which the fraction does not have.
 *
 * The order is asserted rather than arranged. Each escape site in solve.c runs downstream of the arrival site on
 * the same string of the same flow, and both downstream of a delivery (every candidate-arm sink entry returns at its
 * door unless this flow's substitution was performed), so the predecessor is always recorded, except where it
 * matters: the arrival write is refused for a candidate the search's delivery table has contradicted, and an
 * escape past a refused arrival would give that candidate the rank of one that arrived and escaped.
 *
 * A re-delivery is not a demotion. A candidate restarted rather than resumed (decide_enter clears the component's
 * per-flow state) replays from the baseline and delivers again, so this is called with FLOW_RUNG_DELIVERED for a
 * flow already on ARRIVED. The early return is the whole handling: the payload is fixed, so a lower later sample is
 * another look at a fixed answer, never an un-delivery. */
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
    /* And the runway beneath it is complete, by definition rather than observation: a flow cannot hold a rung
       without its predecessor, and rung zero is the approach to the source read this delivery reports at. Written
       here rather than left to dec_replay because the recorded path may run past the source read, and a delivered
       candidate reading a fraction below 1.0 would rank beneath an undelivered one that consumed all of its path.
       Pinned, the bottom bands are disjoint ([0, 0.2] undelivered, [0.4, 0.6] delivered). Unconditional because a
       rung is reached once (the early return), and idempotent because 1.0 is the fraction's ceiling.
       The pair beside it is not pinned: the pin is by definition, so an arm count here would be invented (a
       candidate can reach its source read having replayed nothing). Past this line the fraction is where the
       comparator stands and the pair what was last observed, which is why flow_observe_replay asserts their
       identity only below the delivery. */
    f->cand_replay = 1.0;
    frontier_rank_changed();
}

/* A third kind of quantity, which is neither a ledger nor a comparator and is not read by flow_weight. It is a fact
 * about the path, whether this flow ever took an arm the concrete example contradicts, recorded so that a request
 * the flow builds afterwards declares itself forced rather than derived (a request carries the provenance of its
 * values; see `path_forced` in flow.h for why no value can answer it).
 *
 * It does not touch the frontier generation. A fork is rank-neutral, so both arms of a forced branch are worth
 * what their parent is worth at the branch; re-ranking here would price a property of the arm, the
 * reset-by-splitting this file forbids, and flow_fork_inherit's rank-neutrality equality would not catch it because
 * the quantity is not in the weight. Idempotent by the assignment: a path cannot un-take an arm. */
void flow_mark_forced_arm(void) {
    /* No `if (g_running)` guard, for flow_age_running's reason: every caller marks a flow it stands in. decide.c's
       two decision seams (a bytecode branch, and a native operation's outcome fork over a machine's stated real
       completion) reach here only with a flow switched in, and engine.c's two exploration seams (a declined
       request's failure arm, a modelled close request's arrival) assert `flow_running() == f` before calling. A
       mark with nothing running would leave the path's later requests declaring themselves derived. The standing
       list of callers is flow.h's, beside `path_forced`. */
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

/* The narrower fact about the same path; flow.h's field says why it is recorded at the choice of a witness and not
   at the address that spends it. The guard argument of flow_mark_forced_arm holds here: the one caller, decide.c's
   branch seam, reaches it only having just marked the same path forced. */
void flow_mark_pinned_value(void) {
    DCHECK(g_running != NULL,
           "a determined witness was recorded with no flow running — the determination is a fact about ONE "
           "path, so there is no flow here to be standing on it, and the addresses built from that witness "
           "would be fetched as though their bytes came from the document");
    /* The nesting, asserted at the write and not only at the park. flow.h states this bit is strictly inside
       `path_forced`, and the park's composition asserts the pair, but a mark on an unmarked path would fire that
       assert far from the seam that caused it, naming a park rather than the decision. */
    DCHECK(g_running->path_forced == 1,
           "a flow recorded that it DETERMINED a value on a contradicted arm while its path stands on no "
           "contradicted arm at all — this bit is strictly nested inside `path_forced` and its one caller is "
           "reached only where decide.c has just marked the path, so an unmarked path here is a second "
           "speller of 'did this arm contradict its example' that disagrees with the first");
    g_running->path_pinned = 1;
}

int flow_path_pinned(const Flow *f) {
    DCHECK(f != NULL, "a flow's path was asked whether it determined a witness, of no flow");
    DCHECK(f->path_pinned == 0 || f->path_pinned == 1,
           "a flow's determined-witness mark is neither set nor clear — it is written in exactly one place, "
           "as a constant, so any other value is memory this field does not own");
    DCHECK(!f->path_pinned || f->path_forced,
           "a flow claims to have determined a value on a contradicted arm while claiming to stand on none — "
           "the nesting flow.h declares is what makes reading this a NARROWING of the forced set rather than "
           "a reach outside it, so a consumer given this pair would fire a request the path bit refuses");
    return f->path_pinned;
}

/* Age the running flow by the microseconds of thread time its step just burned. A monopolizer that runs without
   emitting sinks below productive and unrun flows; FLOW_AGE_RATE states the exchange that makes that true. This is
   the only charge on `cpu`: `fam_us` counts every microsecond a family burns as it burns it, so the fork chain is
   charged through the family half below and nothing is handed up at a departure.
   No `if (g_running)` guard: the one caller charges between a switch-in and a finish, so a charge with nothing
   running means the scheduler lost track of which flow burned the time, and discarding it would stop a monopolizer
   aging. */
void flow_age_running(int64_t us) {
    DCHECK(g_running != NULL,
           "thread time was charged with no flow running — the scheduler charges the flow it just stepped, so "
           "this is time nobody is billed for and a flow that never ages");
    /* The scheduler's clock is monotonic, asserted where its reading arrives. A negative charge raises the flow's
       weight, so a flow burning the thread would climb to the top and stay; only a non-monotonic or wrapping clock
       at the call site produces one. */
    DCHECK(us >= 0, "a NEGATIVE thread-time charge reached the WFQ — the aging clock ran backwards, which does "
                    "not slow a monopolizer down, it promotes it");
    /* The account both halves land on, asserted before either reads it: the own half is read over the account's
       silence window (FlowAcct's `emit_gen`), so the first statement dereferences the family pointer. */
    DCHECK(g_running->family != NULL && g_running->family->up == NULL,
           "the running flow's family tag is not the ROOT of a fork tree — an arm whose tag points at its own "
           "node has FOUNDED a family instead of joining its parent's, which is precisely the reset by "
           "splitting the whole of this accounting exists to forbid: it would carry its parent's reward with "
           "none of its parent's aging and outrank the entire backlog for free");
    /* The window this charge belongs to, normalised before it is added to. `Flow.cpu` is a reading only while
       `cpu_gen` matches the account's generation (flow_own_silence); past an emission it holds a previous window's
       arithmetic every reader already answers zero for, and adding to it would charge this flow for time burned
       before its family's last finding. Writing the field to what its readers return is observationally a no-op,
       so it is safe at a site the preempt hook's seam assertion straddles; it is done here because this is a write
       site, and flow_own_silence stays read-only. A stale generation is the ordinary state of every member of a
       family that has just emitted, so this is the normal path, not a repair.
       Whether this member was already away from its family's base (FlowAcct's `away_n`) is read first, because the
       normalise and the charge below can both move it. */
    int was_away = flow_own_silence(g_running) > 0;
    if (g_running->cpu_gen != g_running->family->emit_gen) {
        g_running->cpu = 0;
        g_running->cpu_gen = g_running->family->emit_gen;
    }
    g_running->cpu += us;
    /* And the one transition a charge can make: `cpu` only rises here and the normalise fires only where
       flow_own_silence already answered zero, so base to away is the only direction and a member already away
       stays away. A zero-microsecond charge against a member at base moves nothing, and the test says so rather
       than assuming `us` is positive. */
    if (!was_away && flow_own_silence(g_running) > 0) g_running->family->away_n++;
    /* And the same microseconds on the family. Both are the aging term's (it is their sum), answering two
       comparisons over one window: `cpu` is what this flow has burned since the family last emitted, ordering a
       family's members against each other, and `fam_us` what the whole family burned over that window, ordering
       one family against another. A term with only the second says nothing on a single-family frontier, which a
       real page's is. Both are measured since the family's last emission: a window since the flow's own emission
       is strictly longer, and summing it against a reward credited over the shorter one is the defect FlowAcct's
       `emit_gen` describes. Neither is the optimism term's quantity, a count of completed units charged by
       flow_credit_visit. */
    g_running->family->fam_us += us;
    /* And the same microseconds on this flow's branch bucket: an observation, not a third charge, and nothing in
       flow_weight reads it. This is the one site that adds to either half above, so it is the one site where every
       microsecond can be attributed to a side of a branch without disagreeing with the halves it partitions.
       Lifetime and never forgiven, unlike the halves (silence since the account's last emission, zeroed by
       flow_credit_emit): a bucket answers how much of the thread this side of the branch got, and an arm that
       burned an hour and then emitted did not receive less. The instance total beside it makes the buckets a
       conservation: `sum over live buckets + retired == charged`, asserted at the end of flow_wfq_census. */
    DCHECK(g_running->acct != NULL && g_running->acct->branch != NULL,
           "the running flow has no fork-tree node, or its node names no branch bucket — a live member's node "
           "is minted at flow_new and released only at its departure, so this flow is being charged thread "
           "time that can be attributed to no side of any branch");
    g_running->acct->branch->sub_us += us;
    g_charged_us += us;
    /* The same microseconds went to both halves, which keeps them one reading at two scopes. This is the only site
       that adds to either, so the only one that could add to one alone; a charge reaching `cpu` and missing
       `fam_us` would make an arm's own silence outgrow its family's. */
    DCHECK_AGING_ONE_WINDOW(g_running, "a slice of the thread was charged to this flow and its family");
#if APICLIENT_DEV
    /* And the stamp flow_best's walk compares against, maintained here because this is the one writer of a
       member-half summand that raises no generation: `cpu` is read by flow_own_silence, quantised by
       flow_service_notch and priced into flow_member_key's first term, so the charge above moves the member half
       of the flow it charges while the generation stands still. The walk exempts `m == g_running` at the scan,
       which covers this member only while it holds the thread; flow_switch_out clears the pointer and leaves the
       charge in the key, so without this stamp the first scan after a slice boundary would meet a non-running
       member whose key moved by whole FLOW_AGE_QUANTUM steps. flow_silence_phase's decomposition permits exactly
       this: `k` "moves only when the flow being CHARGED crosses a quantum ... between two dispatches this moves for
       AT MOST ONE MEMBER of the frontier", so the stamp restores the claim it makes and every other member still
       fires. One re-key per dispatch is O(1), so an index over this key updates here and needs no walk. After both
       charges and the window assert, so the stamp is the key the member now carries; read-only on its operands. */
    g_running->key_last    = flow_member_key(g_running);
    g_running->key_gen     = g_gen;
    g_running->key_stamped = 1;
    /* And the per-member half, in the same group, because this charge moves it by the same `k`: flow_index_key
       reads flow_service_notch, the floor this line advances, so a charge crossing a quantum moves both stamps and
       one inside a quantum neither. Stamping only the sum would leave the half's check firing on the member the
       charge is about. */
    g_running->ikey_last   = flow_index_key(g_running);
    /* And the phase, for the same reason: this charge is the one writer of `own_silence` that raises no
       generation, so it is the one writer all three stamps must survive, and the phase is the half the key cannot
       carry (the key reads the floor; a sub-quantum charge moves only the remainder). See solver/flow.h's
       `phase_last`. */
    g_running->phase_last  = flow_silence_phase(g_running);
#endif
    /* That a slice of the thread moves the rank it is charged to is asserted where the charge meets the pick, in
       engine.c's scheduler loop, the one place both are visible; the claim is about their order. FLOW_SILENCE_US
       states what a charge the pick cannot see costs. */
}

/* Is this member between units of work, asked of any member and not only the one holding the thread (which is why
   it is a function here rather than engine.c's local). HTML §8.1.4.4 "Calling scripts", clean up after running
   script step 3: "If the JavaScript execution context stack is now empty, perform a microtask checkpoint". A live
   frame answers no (`Flow::frame` is that stack, solver/flow.h), and so does an owed checkpoint, which is a step of
   the turn that is ending. These are the two clauses flow_credit_visit asserts, asserted through this predicate so
   the readings cannot drift.
   It is narrower than engine.c's `unit_ended` by one clause that is not a per-member fact: that local asks
   `!JS_HasParkedFlow(JS_GetRuntime(ctx))` of the runtime, because the running flow's parked-continuation queue is
   moved into the runtime for its turn (flow_switch_out's JS_TakeParkedFlows stores it back on `Flow::parked`). The
   residual at the counter below states what that costs. It decides nothing and moves no rank: flow_weight does not
   read it, no fork carries it, and its consumers are an assert and a census counter. */
static int flow_between_units(const Flow *f) {
    return f->frame == NULL && !flow_job_microtask(f);
}

/* The third clause of engine.c's unit boundary, asked of a member that is not holding the thread. `Flow::parked`
   holds a member's parked continuations and is authoritative for every member except the running one, whose queue
   is in the runtime for its turn, so `parked` reads NULL for it however many it holds. It asserts rather than
   guards: answering "no park" for the running flow would be plausible and wrong, and a caller that needs it needs a
   runtime handle (passed at the pick), which is a capability to build. The counter below never asks about the
   running member (its raise requires `best != seed`, and the seed is the incumbent), and this assert makes that
   checkable: a fire means the seed was not the running flow and the residual's population argument is wrong. */
static int flow_holds_park(const Flow *f) {
    DCHECK(f != g_running,
           "a member's parked continuations were asked for while it holds the thread — its queue is in the "
           "RUNTIME for the duration of its turn, not on Flow::parked, so this would answer NO PARK for a "
           "flow that has several. Build the runtime handle at the pick (engine.c asks JS_HasParkedFlow of "
           "the runtime) rather than reading a field that is empty by construction here");
    return f->parked != NULL;
}

/* A flow completed a unit of work: the optimism term's quantity, a count rather than a clock (flow.h's `visits`
   says what a unit is and why the term may not be thread time). A separate call from flow_age_running though
   charged at the same moment by the same caller, because they measure the two quantities the scheduler names
   separately; a step that burned a quantum inside a program charges the first and not the second. The flow is not
   asserted to be the running one (the finish path can have cleared `g_running`); what is asserted is the property
   that makes the count a trial count. */
void flow_credit_visit(Flow *f) {
    DCHECK(f != NULL, "a completed unit of work was credited to no flow — the optimism term's whole quantity is "
                      "per flow, so a charge with no owner is a turn nobody is recorded as having had");
    DCHECK(f->frame == NULL,
           "a flow was credited a completed unit of work while it is still INSIDE a program — a preempt is the "
           "middle of a trial, not the end of one, and counting it makes this term a second clock beside the "
           "aging: the flow is then outranked by its own arms after one quantum and no program ever finishes");
    /* And an owed microtask checkpoint is the same sentence, and is where the unit ends. HTML §8.1.4.4 "Calling
       scripts", clean up after running script step 3: "If the JavaScript execution context stack is now empty,
       perform a microtask checkpoint": the checkpoint is a step of cleaning up after the script, in the same turn,
       and §8.1.7.3 "Processing model" places the identical step at the end of a task. The checkpoint runs "While
       the event loop's microtask queue is not empty: … Run oldestMicrotask", so the turn is over when the queue is
       empty; an empty execution context stack is the checkpoint's trigger, not the unit's end.
       The credit is a demotion (the optimism term is 1/(1+visits)), so crediting at the trigger would demote a flow
       at the instant its queued reactions became eligible (every job arm of flow_step is under `frame == NULL`,
       engine.c), and it would have to win the thread again against the whole frontier before its checkpoint ran;
       on a forking page the frontier refills faster than it drains, and no job runs.
       It is not a drain loop: each microtask is still its own flow_step return, preemptible, parkable and
       re-ranked. This removes only the demotion across the checkpoint, so flow_pick's strict comparison leaves the
       thread with the flow on a tie and any strictly better flow still takes it. A checkpoint that never ends is
       governed by the aging term, not by a cap. */
    DCHECK(!flow_job_microtask(f),
           "a flow was credited a completed unit of work while it still owes a MICROTASK CHECKPOINT — HTML "
           "§8.1.4.4 \"Calling scripts\" step 3 of clean up after running script performs the checkpoint as "
           "part of the turn that just ended, so the unit is not over and this credit DEMOTES the flow at the "
           "one instant its queued reactions became eligible to run: it must then out-rank the whole frontier "
           "a second time to run its own checkpoint, which on a forking page never happens");
    /* And the two above are read through the predicate a second consumer shares, so the boundary cannot gain a
       clause in one place without the other. The two specific asserts stay because each names its cause; this one
       pins that they are the whole of flow_between_units' boundary, which the starvation counter relies on. An
       identity under the code as it stands, so it fires only on an edit that breaks it. */
    DCHECK(flow_between_units(f),
           "a completed unit of work was credited to a flow that flow_between_units says is INSIDE one, while "
           "the two asserts directly above passed — the unit boundary has gained a clause in one of the two "
           "places that state it and not in the other, so the census row that counts a re-dispatch as having "
           "had nothing to continue is judging a different boundary than the optimism term is credited at");
    f->visits++;
    /* And the order is told, because this credit lowers the optimism term and so is a rank change. Every writer
       that moves a member's rank raises the generation; engine.c's rescan hook rests on the claim that a
       per-member term cannot move between two generations, and its DCHECKF prints the five terms side by side so
       a missing raise shows as a `completed units` pair differing with every other term equal. Raised at the
       writer rather than tolerated by a fifth clause in that assert, which would let the hook rank against a
       state the scheduler had not used. Not a bound or a second clock: frontier_rank_changed raises the yield
       request, and raising is not deciding. The rate is one per completed unit, the rate at which this term moves.
       This record retires when the optimism term no longer reads a per-member count. */
    frontier_rank_changed();
}

/* The scheduler chose this member. flow.h's `picks` says why a dispatch count is the one statement about a member
   an emission cannot erase, and `never_picked` the reading it makes possible. It decides nothing and moves no
   rank: flow_weight does not read it, no fork carries it and nothing resets it, which keeps it a measurement. If it
   ever acquired a consumer inside the ordering, flow_fork_inherit's rank-neutrality equality would fire. */
void flow_credit_pick(Flow *f) {
    DCHECK(f != NULL, "the scheduler credited a dispatch to no flow — this is the one point every context "
                      "switch converges on, so a NULL here is a switch that ran with nothing switched in");
    DCHECK(flow_is_member(f),
           "the scheduler credited a dispatch to a flow that is not a member of the frontier — the census row "
           "this feeds is taken over the members, so a departed flow's dispatch would be counted nowhere and "
           "the starvation reading would be a fraction of the wrong population");
    f->picks++;
    /* And the same event counted for the life of the instance: a member that departs takes its share of the
       per-member total with it, so the sum over standing members is a gauge and a difference of two gauges is not
       a number of dispatches. Counted on the one line every dispatch passes. */
    g_picks_total++;
    /* And the subset that reached a member with an empty JavaScript execution context stack; see
       `g_unframed_picks_total` for why the census's gauge cannot answer this and why the increment is in the same
       block as its total. The predicate is the one flow_checkpoint_due and the census's job and delivery arms
       call, so the numerator and its population move together. It reads the same on either side of the switch:
       engine.c credits this after flow_switch_in, which writes the delta, the decision cursor, the pins and the
       ranked-at terms and touches neither `frame` nor the program cursor this predicate reads. */
    if (flow_stack_empty(f)) {
        g_unframed_picks_total++;
        /* And the ready arm's own three conjuncts (see `g_ready_picks_total`), asked here rather than at the
           census. The cheap boolean precedes flow_job_pending's property read, so the read is paid per unframed
           dispatch; `flow_stack_empty` is the conjunct the enclosing `if` answered. */
        if (!flow_host_owed(f) && flow_job_pending(f) > 0) g_ready_picks_total++;
    }
}

/* Is this flow's JavaScript execution context stack empty? HTML §8.1.4.4 "Calling scripts", clean up after running
   script step 3: "If the JavaScript execution context stack is now empty, perform a microtask checkpoint." That is
   the precondition of a microtask checkpoint and of a task alike (§8.1.7.3 "Processing model" runs one task and
   then performs a checkpoint, so neither begins part-way through a program). `Flow::frame` is that stack ("the
   current script's live preemptible frame, NULL between scripts", solver/flow.h), so a live frame answers no.
   The other half is the row at the cursor: §4.12.1.1 "Processing model" ends "prepare the script element" with
   "Otherwise, immediately execute the script element el, even if other scripts are already executing", so that
   program runs inside the one that inserted it and the stack has not emptied across it. Every other program the
   flow has left is a task (the next <script>, a lazy chunk, a `javascript:` URL, a §8.7 Timers string handler, a
   peer's operation).
   One predicate for every consumer of the sentence: the microtask checkpoint, the networking task source's arm in
   flow_step, the engine's reply-delivery arm, and the census. cold_census asks it directly (solver/cold.h's
   `stack_empty`): `flows - framed` answers only the first half and is an upper bound, so this count is what
   separates replies members cannot take from an arm not reached for another reason. */
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

/* A forked sibling is a continuation, not a newcomer, so it inherits every term of its parent's account; this is
 * the one place that says so. A fork appends an arm to a decision vector (flow.h): the sibling holds the parent's
 * frame snapshot, COW delta, DOM base, queued jobs and outstanding replies, and the constructor's zeros on the
 * ranked terms would each be a false statement that promotes it:
 *
 *   the reward: the sibling has executed every emission the parent made before the branch (they are in its delta
 *   and its recipe replays them). It is not copied either: a copy is neutral against the arm born at this instant
 *   but makes two arms of one parent forked at two instants differ by what the parent emitted in between, which
 *   is birth order. The reward lives on the fork family (FlowAcct's `val`) and the arm joins it below.
 *   `cpu`: the sibling has burned the parent's thread time on the prefix it resumes. A zero would hand a flow
 *   running since boot the silence of one that just emitted, so a forking loop (the `Array.from(state.items)`
 *   walk FLOW_AGE_RATE was written against) would hand its debt to a debt-free child and never sink.
 *   `visits`: the sibling has completed every unit its parent completed. A zero would hand it the full optimism
 *   bonus (1.0) reserved for a flow never picked, a reset-by-splitting the aging cannot reach, since a flow inside
 *   a program completes no unit.
 *
 * Inheriting puts the fork at the back. The sibling ties with its parent, so it does not dislodge it (flow_pick
 * seeds the incumbent and compares strictly), and ranks below every flow that has consumed less service, which is
 * the backlog: start-time fair queueing's rule that a continuation enters at its flow's virtual time, or any flow
 * resets its clock by splitting.
 *
 * Never-starved holds among members of equal reward, and the family is what makes reward equal: every member of a
 * family reads the same reward, so within a family an arm's silence is frozen at its fork while the flow that forked
 * it goes on burning, an unrun arm ranks at or above its maker, and the queue drains oldest-arm-first. An emission
 * forgives the whole account's window (FlowAcct's `emit_gen`) and does not touch `visits` (flow_credit_emit), so
 * it breaks neither axis. One writer does move a parent away from its frozen arms, as intended:
 * flow_observe_survival and flow_observe_rung raise the parent's fitness while the arm's stays where it was born, a
 * flow that just carried its payload a rung further being the one the frontier should run. So the guarantee holds
 * "for as long as that flow does not advance its search". The arm's frozen `cpu` is forgivable, because the own
 * half is read over the account's window. Between families the reward orders what it is for.
 *
 * Which state a run is in is a measurement: flow_wfq_census reports the reward spread and how many members have
 * emitted themselves, emitted as @WFQ beside @PROGRESS and @COLD, and a spread above 1.0 is a statement about
 * several families. The family join below carries both the reward and the aging's family half: a fork chain is one
 * monopolizer wearing N names, so it enters, is charged and is credited as one thing. See FlowAcct. */
void flow_fork_inherit(Flow *sib, const Flow *parent) {
    DCHECK(sib != NULL && parent != NULL && sib != parent,
           "a fork's accounting was inherited from nobody, or from itself — this is the one line that decides "
           "where a newborn arm enters the queue, so it cannot be asked about one flow");
    /* Nothing may have been credited or charged first, because the inheritance assigns both terms: a sibling run or
       credited before receiving its account ran at a rank nobody chose, and the assignment would erase the result. */
    DCHECK(sib->val == 0.0 && sib->cpu == 0 && sib->cpu_gen == 0 && sib->visits == 0 && sib->picks == 0 &&
           sib->cand_replay == 0.0 && sib->cand_replay_of == 0 && sib->cand_replay_arms == 0 &&
           sib->cand_surv == 0.0 && sib->cand_rung == 0 && sib->path_forced == 0 &&
           sib->path_pinned == 0 &&
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
    /* And it may not already stand under a flow in the fork tree: flow_new mints a root node, so a non-NULL `up` is
       a second inheritance, leaking the first parent's node and charging every arm it sheds to the wrong chain. */
    DCHECK(sib->acct != NULL && sib->acct->up == NULL,
           "a forked sibling already stood under a flow in the fork tree — it is being given a second parent, "
           "so the service it sheds when it departs is charged to a chain it was never part of");
    /* The reward is not copied here; the family join below carries it, one pointer read by every arm, which is why
       the equality at the end holds over a term this function never names. `Flow.val` is what one member emitted,
       never inherited and unranked, so the precondition requires it to be zero rather than assigning it. */
    sib->cpu = parent->cpu;
    /* And the window that reading belongs to: `Flow.cpu` is the burn since the account last emitted and is read only
       while `cpu_gen` matches the account's generation (flow_own_silence). The arm joins the parent's family below,
       so copying the mark makes the arm read exactly what the parent reads; copying `cpu` alone would hand an arm of
       a just-emitted parent the parent's previous window's burn, a debt already forgiven, and the equality at the
       end fires on it. */
    sib->cpu_gen = parent->cpu_gen;
    /* And the completed units, the optimism term's coordinate: an arm has finished every program, job, delivery and
       lifecycle stage its parent finished before the branch, because it is that execution with one more arm on it.
       The aging cannot correct a zero here, because a flow that never finishes a unit never decays. */
    sib->visits = parent->visits;
    /* And the distance, the term that is read rather than accumulated. An arm of a candidate carries the same payload
       to the same sink (engine.c copies the substitution to the sibling), so it stands where its parent stood: the
       bytes that survived to a sink, and the rungs they reached, did so on the prefix both share. A zero would let a
       candidate refresh its rank by branching, and the equality below catches it. For a non-candidate parent this
       copies zero to zero. The equality is written over flow_weight rather than a list of fields because the
       fitness is several fields: a list that missed the rung would let a candidate that had arrived and escaped
       re-enter as one that merely survived, worth two thirds of the comparator's range. */
    sib->cand_replay = parent->cand_replay;
    /* And the pair that fraction is the halves of, carried for a different reason: nothing in the order reads the
       pair, so the equality cannot see it. What forces it is the identity flow_observe_replay asserts on its next
       arm: a sibling holding its parent's fraction over a zeroed pair would make two contradictory statements about
       one sample. It is also true: the arm has replayed that prefix. Two arms of one parent forked at two instants
       read it the same, because a flow that forks has stopped replaying (decide.c reaches its fork arm only with the
       cursor at the end of the vector, or after dec_leave_path has truncated it there), so a parent consumes no arm
       between two of its own forks. */
    sib->cand_replay_arms = parent->cand_replay_arms;
    sib->cand_replay_of = parent->cand_replay_of;
    sib->cand_surv = parent->cand_surv;
    sib->cand_rung = parent->cand_rung;
    /* And the family it joins: the term the aging reads and the term the reward is, which makes "a fork chain is one
       monopolizer wearing N names" true of the arithmetic. flow_new minted the arm a root (the precondition checks
       it) and this points the arm at its parent's; every microsecond and every finding any arm produces is then
       charged or credited once, to this node, and read by every arm. It carries two of the equality's terms.
       The arm's own node is still the fork edge, refcounted: `acct`/`up` is the ancestry (who forked whom, and what
       keeps a root addressable), `family` the account. Collapsing them would mean walking the ancestry on every
       flow_weight, which is evaluated inside DCHECK conditions where a compressing walk is forbidden. */
    sib->family = parent->family;
    /* And a newborn that inherited a burn is born away from its family's base (FlowAcct's `away_n`). The family
       pointer is only now in place, so this is the first instant flow_own_silence can be asked of this member; a
       fork off a parent at base mints a member at base, hence the test. */
    if (flow_own_silence(sib) > 0) sib->family->away_n++;
    /* And the edge itself, which makes a fork chain an accounting unit: the newborn's place in the ranking is under
       the flow it branched from. */
    sib->acct->up = parent->acct;
    parent->acct->refcount++;
    /* And the branch bucket this arm's subtree is counted in, the third accounting scope, derived from the edge
       above. A fork off a root is a new top-level arm and keeps the bucket flow_new opened for it; any other fork
       joins its parent's. So exactly one node per ancestry path answers `branch == self`, and every member reaches
       its bucket in one indirection.
       The membership moves as a departure and an arrival in one statement: both counters are lifetime, and a census
       can land between flow_new and here (flow_new's allocations may page a flow out), so retracting the `sub_born`
       flow_new published would make a lifetime counter fall. The sibling's own node is left at born == gone, a
       bucket no census reaches because nothing points `branch` at it.
       The transfer is load-bearing for the rank-neutrality equality: flow_nonreward sums flow_branch_bonus, which is
       `1.0 / sub_born` over the bucket the arm moves into, and both sides read that bucket after the increment.
       Without the transfer the newborn keeps a bucket at one member (bonus 1.0) against a parent bucket of L, and
       the equality fires for every L above one. The root arm passes for a different reason, asserted below. */
    sib->acct->depth = parent->acct->depth + 1;
    {
        FlowAcct *br = parent->acct->up ? parent->acct->branch : sib->acct;
        DCHECK(br != NULL && sib->acct->branch == sib->acct && sib->acct->sub_born == 1 &&
               sib->acct->sub_gone == 0,
               "a forked sibling reached its inheritance without the branch bucket flow_new opens for every "
               "node, or already carrying membership in one — the transfer below is a departure and an "
               "arrival of ONE member, so a sibling arriving with any other count publishes a bucket total "
               "that no flow corresponds to and the census's membership identity fails on a healthy frontier");
        /* The root arm's neutrality, asserted where it is decided. Where the arm keeps its own bucket the two sides
           share no denominator, so flow_branch_bonus is equal across them only while both buckets stand at the
           `sub_born == 1` flow_new wrote. The arm's is one by the precondition above; the parent's is one only
           because `br` is never `parent->acct` when the parent is a root. That is a property of this expression,
           not of any field, so a bucket policy that let a root be some node's `branch` would leave every other
           check green and make a fork off the root a promotion of up to a full point. It names `sub_born` and
           `sub_gone` separately rather than their difference, because the term reads `sub_born`: a live count of
           one is satisfied by a bucket at (born 100, gone 99), which would read 1/1 against 1/100. Unlike the
           equality below, which compares two sums any cancelling pair satisfies, this names the structural fact,
           and it is false for any frontier in which a root has been forked into. */
        DCHECK(br != sib->acct ||
               (parent->acct->branch == parent->acct &&
                parent->acct->sub_born == 1 && parent->acct->sub_gone == 0),
               "a fork left the newborn in its own branch bucket while the parent's bucket has minted or shed "
               "past the one member flow_new wrote — so the two sides of this branch read different "
               "`1/sub_born` from flow_branch_bonus and branching just changed a flow's own rank, which is "
               "the one thing the WFQ may never let it do");
        /* And the bucket's offset moves on the statement that moves the counter it is a function of: the
           `br->sub_born++` re-ranks every live member of the bucket the arm joins, the term is read out of
           `sub_offset`, and an increment that left the offset standing would let the equality compare the old
           denominator against itself and pass. Adjacent, the pair cannot come apart without acct_branch_offset's
           guard firing at the next weighing. The departure, the arrival and the re-pointing are the body of one
           `if`, so a census landing outside it reads a coherent frontier. */
        if (br != sib->acct) {
            sib->acct->sub_gone++;
            br->sub_born++; br->sub_offset = 1.0 / (double)br->sub_born;
            sib->acct->branch = br;
        }
    }
    /* And the path's forced mark, the one inherited thing the ranking never reads: an arm is its parent's path with
       one more arm on it, so it stands on everything the parent's path stood on. A sibling born clean of it would
       declare its requests derived on the strength of being forked, the provenance form of reset-by-splitting, and
       forcing is what makes most siblings. It is not in the equality below, since `path_forced` is not a weight
       term; the precondition (the sibling has decided nothing yet) and the equality (nothing entered the weight
       unnoticed) are what bracket it. engine.c grades every request built past it PROV_FORCED rather than
       PROV_DERIVED, so the engine carries per flow the bit separating the arm a real browser would take from an arm
       this solver forced, and uses it to label, never to order. A weight term reading it would differ across the two
       arms of a forced branch (the arm taken against the example is marked, its sibling is not) and fire the
       equality below, so ordering by it is a decision about what fork-neutrality covers, every arm or every arm the
       run did not contradict, to be made in the scheduler's rule and not inside a weight term. */
    sib->path_forced = parent->path_forced;
    /* And the narrower fact with it, for the same reason: a witness the parent had chosen is one the arm computes
       from too, and a sibling born clean of it would claim its addresses rest on the document's own bytes. Not in
       the equality, for `path_forced`'s reason. */
    sib->path_pinned = parent->path_pinned;
    /* The scheduler's one WFQ, asserted at the fork: a fork is rank-neutral. At the instant it is born the sibling is
       worth exactly what its parent is worth. The equality fires the moment anything enters flow_weight that a fork
       does not carry, which is the shape of every banned fix (a depth bonus, a script-index term, a separate visit
       counter, a "prefer flows that have run" tiebreak), here at the fork rather than later in a progress line
       reading `finished 0`.
       flow_weight is `fam_val + (cand_replay + cand_surv + cand_rung)/FLOW_RUNGS_N + 1/(1+visits) + 1/sub_born
       − (own_silence + fam_us)*RATE`, so a fork carries nine things through eight assignments: `cand_replay`,
       `cand_surv` and `cand_rung` (the runway fraction is a fact about the path both arms share, and
       decide_fork_blob hands the sibling the parent's cursor); `visits`; `cpu` and `cpu_gen` (a reading and the
       window it is a reading of); the family tag, joined, which carries both the reward and the aging's family
       half; and the bucket transfer, which carries `1/sub_born`. Each addition to the formula has arrived through
       this line firing. Had the arm kept its fresh root, its `fam_us` would be 0 against a family that has burned
       and its `val` 0 against one that has emitted, and this would fire at the first fork.
       It is a claim about one instant: it compares the arm born here against the parent here, so it cannot see two
       arms of one parent forked at two instants, which a term copied at the fork makes differ by birth order. A term
       read through the shared root is equal across every arm at every instant, so any future term whose value can
       move between two forks of one parent belongs on the account, not on the flow. */
    /* And the aging's two halves are one reading for the arm, which the equality cannot say: it compares a sum, so it
       passes however the halves are split, including the split an arm is born into if `cpu` is carried without
       `cpu_gen`. This is the site that mints that population, so the relation is asserted on the newborn. */
    DCHECK_AGING_ONE_WINDOW(sib, "a fork handed this arm its parent's account");
    DCHECK(flow_weight(sib) == flow_weight(parent),
           "a fork changed the ranking — the sibling is the parent's path with one arm appended, so the two "
           "must be worth the same at the instant of the branch; a weight term the fork does not carry lets a "
           "flow change its own rank by branching, which is the one thing the WFQ may never let it do");
}

/* Release one flow (flow.h): the one place every field a Flow owns is given back. */
void flow_release(JSContext *ctx, Flow *f) {
    /* The flow is not switched in. Everything below is released as parked state: the heap delta's head is freed
       rather than unapplied, the DOM head's created nodes are destroyed as detached, and the decision and pin state
       is read out of blobs rather than decide.c's live globals. All three are false for the running flow. */
    DCHECK(f != g_running,
           "the RUNNING flow was released — its heap delta, its document writes and its decision state are all "
           "live rather than parked, so this frees one copy of each while the scheduler still holds the other");
    /* A paged flow is not a dropped one. Five things a released flow can still carry are work items on the one
       frontier: a parked continuation, the suspended frame (its whole heap-frame chain), replies the host still owes
       it, queued jobs and a peer's routed deliveries. Dropping a work item is forbidden
       unless the flow was written to the cold tier, whose recipe replays the code that created each of them (the
       reactions are re-enqueued, the requests re-issued, and deliveries are in the recipe as 'm' records). So a flow
       not written out may hold none of them, and they are one assert, not one assert plus silent frees. It asks the
       flow, not the engine: `engine_frontier_paged` answers for the whole session, and a partial self-park writes a
       tail while flows above it run on unwritten, so only the per-flow answer is exact.
       The smoke host's give-up path fires this: a provider that answers nothing ends run_scheduler, main.c tears the
       instance down, and flow_registry_free releases survivors suspended mid-frame with replies outstanding. The
       abort names the flows a run threw away at the moment it threw them. */
    DCHECK(f->paged || (f->parked == NULL && f->frame == NULL && flow_job_pending(f) == 0 &&
                        pending_count(f->pending) == 0 && flow_deliver_pending(f) == 0),
           "a flow was released holding WORK — a parked continuation, a suspended frame, queued reactions, "
           "replies the host still owed it, or a peer's routed message — and it was never written to the cold "
           "tier, so nothing replays it and every one of those is a work item the ONE frontier just dropped");
    /* And the parked continuations are released here, because the flow owns them: every park takes a reference that
       only the resume gives back. Each park's disposer rides the park (quickjs.h's JSFlowParkFreeFn), so this calls
       one function over the set. `paged` does not excuse it: the assert above is about a dropped work item, while
       the memory the continuations hold is freed by no recipe and is invisible to the gc_obj_list walk (neither the
       activations nor their TrampFrames are GC objects). That every member carries a resume, a disposer and a realm
       is asserted over the whole ring in JS_PutParkedFlows and JS_FreeParkedFlows.
       A program this flow was still parsing is not behind `paged`: a suspended compile is re-derivable from the
       row's bytes, so the memory is this flow's to give back either way. It is released through the engine's entry
       because it holds the descent's state (every frame's atoms, the chunk array, the token and the JSFunctionDef
       chain). A no-op on a NULL slot. */
    JS_FlowCompileDrop(ctx, &f->compile);
    JS_FreeParkedFlows(f->parked);
    f->parked = NULL;
    /* `frame` is the JS_FlowNew handle holding this flow's whole heap-frame chain (every activation, closure and
       local it is suspended across), so leaving it retains the entire realm, and the runtime's leak walk reports a
       Window, a context and many anonymous Functions with nothing naming the owner. */
    if (f->frame) { JS_FlowFree(ctx, (JSValue *)f->frame); f->frame = NULL; }
    /* The replies it was still owed: the register is one JS value, so its resolve capabilities, reply values and
       strings go with one release. */
    pending_free(ctx, &f->pending);
    /* The unstarted operations: one release for the array and every [record, token] pair. That they are gone is
       asserted by the caller (flow_finish, the pager), since a dropped record is a peer suspended for ever; this is
       the release, not the check. */
    JS_FreeValue(ctx, f->perform_q); f->perform_q = JS_UNDEFINED;
    /* And the routed deliveries, one release for the array and every [record, senderOrigin] pair; that they are gone
       is asserted above and by the caller. */
    JS_FreeValue(ctx, f->deliver_q); f->deliver_q = JS_UNDEFINED;
    /* And which sending timelines it was in, one release for the array and every [vector, taken, arm] row. Not on
       the work-item list above: a commitment is not work owed to anybody but what this timeline was, and a flow
       that has ended has no timeline left to be wrong about. */
    JS_FreeValue(ctx, f->deliver_world_q); f->deliver_world_q = JS_UNDEFINED;
    cow_delta_release(ctx, (CowDelta *)f->delta); f->delta = NULL;
    /* The head before the chain below it: a head node inserted under a segment's node is that node's child, and a
       child is freed before its parent is freed deep. */
    if (f->dom) dom_buf_free(f->dom, f->dom_n);
    f->dom = NULL; f->dom_n = f->dom_cap = 0;
    dom_base_release(f->dom_base); f->dom_base = NULL;
    decide_blob_free(f->dec_blob); f->dec_blob = NULL;
    concolic_pins_blob_free(f->pin_blob); f->pin_blob = NULL;
    /* The body column gives a reference back rather than freeing: a program's text is shared by every timeline
       holding it (solver/dyn_body.h), and the last flow to leave frees it. The other two columns are this flow's
       own strings. */
    for (int k = 0; k < f->dyn_n; k++) { dyn_body_unref(f->dyn[k]); free(f->dyn_token[k]); free(f->dyn_url[k]); }
    free(f->dyn); f->dyn = NULL;
    free(f->dyn_cand); f->dyn_cand = NULL;
    free(f->dyn_type); f->dyn_type = NULL;
    free(f->dyn_url); f->dyn_url = NULL;
    /* The element column frees only itself: a row's `script` element is a borrowed node of the document's tree,
       owned by that tree or, for a node a flow created, by the flow's DOM delta, which dom_buf_free discarded. */
    free(f->dyn_el); f->dyn_el = NULL;
    free(f->dyn_doc); f->dyn_doc = NULL;
    free(f->dyn_id); f->dyn_id = NULL;
    free(f->dyn_run); f->dyn_run = NULL;
    free(f->dyn_token); f->dyn_token = NULL;
    free(f->dyn_pos); f->dyn_pos = NULL;
    f->dyn_n = f->dyn_cap = 0;
    /* And the undrained microtasks and tasks, one release for the array and every record; that they are gone is
       asserted at the top of this function. */
    JS_FreeValue(ctx, f->jobs); f->jobs = JS_UNDEFINED;
    /* And its world dies with it, the one state a released flow leaves in another instance's heap. A peer that was
       ever posted to or read by this flow holds a COW segment keyed on its world, materialized on arrival and
       released only when told, so an unannounced departure is a segment that peer carries for the rest of its
       process, and a peer holding one cannot park (cold.c refuses: a foreign segment has no recipe of ours). The
       ancestors whose last live descendant this flow was die with it, hence a list. */
    {
        const char *const *gone;
        int n = world_flow_gone(f->world, &gone);
        engine_notify_worlds_gone(ctx, gone, n);
    }
    flow_remove(ctx, f);
}

void flow_registry_free(JSContext *ctx) {
    /* A flow left in the frontier is the ordinary case, not an error: the session closes over its survivors by
       design (engine_sched_step's exhausted path leaves every host-owed flow alive, and in the product a parked
       flow outlives the session). The teardown for a flow that never finished is the same operation as evicting
       and finishing one, so it is one call.
       Who left them is recorded at each release, because by the assert below every owner is gone and the census
       there only says frames survived. A flow's heap chain is its own (JS_FlowClone deep-copies), so a count that
       does not drop when a suspended flow is released is a fact about that flow. It records rather than asserts
       and calls nothing: flow_step's JS_FLOW_DETACHED arm hands a base to an awaited promise and leaves `frame`
       NULL on purpose, so "a released flow must drop a frame" is not claimable here; it reads four fields it owns
       and carries them into the existing abort. */
#if APICLIENT_DEV
    int rel_i = 0, culprit = -1, culprit_frame = 0, culprit_park = 0, culprit_paged = 0, culprit_started = 0;
    JSRuntime *rrt = JS_GetRuntime(ctx);
#endif
    while (g_flows_n) {
        /* And which arm of `g_departures` this one is, raised on the line that performs it and above the
           dev/release split because the departure happens in both. See g_departures_teardown. */
        g_departures_teardown++;
#if APICLIENT_DEV
        Flow *rf = g_flows[0];
        int had_frame = rf->frame != NULL, had_park = rf->parked != NULL;
        int paged = rf->paged, started = rf->started;
        int before = JS_TrampFrameCount(rrt);

        flow_release(ctx, rf);
        /* The last suspended flow whose release freed no frame; the newest candidate wins, because a chain that
           outlives the frontier was left by the last release that failed to take one. */
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
    /* And the world namespace goes down after the flows: releasing a flow announces the death of its world, and the
       registry is what names that world. Any segment this instance still holds for a peer's world is a foreign
       flow's malloc'd state that nothing else frees and the GC walk never names. */
    world_registry_free(ctx);
    /* Not one heap call frame and not one step machine may outlive the frontier, and this is the one point where
       that is decidable. A TrampFrame and a suspended builtin's state are the two largest things in the @HEAP
       line's `unattributed` residual, invisible to gc_obj_list (neither is a GC object), and each belongs to some
       flow's suspended chain, so with every flow released the census must be zero. This makes it a gate. */
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
    /* And not one node of the fork tree: every reference on a node is its own flow's or a live descendant's, and
       both are gone. This makes acct_compress_dead's re-pointing checkable (a node it re-pointed and failed to
       release holds the ancestry above it, and a node is not a GC object), and it catches a family root kept alive
       by a `family` tag nobody dropped: a departing flow clears its own (acct_depart), so a survivor names the exit
       that did not. */
    DCHECK(g_acct_live == 0,
           "a FORK-TREE node outlived the frontier — its only references are a released flow's own and a live "
           "descendant's, so one still live is a reference nobody dropped and the whole ancestry it names is "
           "unreachable memory");
    /* And not one frozen segment. A segment of the heap or document chain is referenced only by the deltas forked
       below it and the segments above it, the world registry's foreign segments and the frontier's flows, both
       released above, so the count must be zero, as decide_free asserts for the third chain on the same primitive.
       A non-zero count is a delta reference nobody dropped, holding no live JSValue and invisible to gc_obj_list.
       This makes cow_delta_release's and dom_base_release's refcount arithmetic checkable. */
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
    /* And the decision state the frontier stands on, released here and not by each host. Every flow's parked vector
       is a reference on a shared frozen chain, and the running flow holds one more in decide.c's globals; the blobs
       went with the flows. A line in each of the three hosts' teardowns would be a hand-copied list a host can
       forget. The flows are not the only holder: an open @S search holds one (solve.c's add_pending freezes the
       path its candidates are re-injected at), and that blob belongs to the search, which outlives the flow that
       detected it. Its give-back is solve_free, so solver_agent_free calls that before this function, or
       decide_free's assert fires. */
    decide_free();
    /* And the path constraint the same decisions were made under, the fourth chain on the same refcounted immutable
       segment. Its blobs went with the flows; the one reference left is the globals', held by whichever flow was
       switched out last, and without this release a session leaks that flow's frozen constraint, its entries and
       every key and value string, with nothing naming an owner. Released here for the decision chain's reason. */
    concolic_clear_pins();
    {
        long segs = 0, entries = 0, bytes = 0;
        concolic_chain_stats(&segs, &entries, &bytes);
        DCHECK(segs == 0,
               "a frozen CONSTRAINT segment outlived the frontier — every reference on that chain is a parked "
               "flow's blob or the running flow's globals, and both are gone by here, so one still live is a "
               "blob nobody released and the whole narrowing it names is unreachable memory");
    }
    /* And the park document written out of this frontier, the frontier's own residue, released here for the same
       reason: the host has read it out of the result document before any teardown runs. */
    cold_free();
    /* And the ledger count the ask census cold_free just reset is read against, reset on the next line: the two
       are comparable only while they describe one document. The flows are already gone, so no member holds a row
       this stops counting. */
    g_world_commit_rows = 0;
    /* And the pending register's interned field names, here for the same reason: the corpus hosts take a runtime
       down and bring another up per file, so an atom left here is a handle into a freed table the next session's
       first push would write through. */
    pending_free_ctx(ctx);
}

/* Every flow ever created, reported beside the switch count: without it "the frontier grew" and "each flow got
   slower" look identical, and they need opposite fixes. */
static long g_flows_created;

/* Is every world minted from here on owed an image? flow.h's `flow_paint_every_world` holds the argument and why it
   is one-way. Read at the one constructor below, so a mode turned on mid-run covers the members born after it and
   the frontier walk covers those already standing. Zero is the answer for a host that never asked. */
static int g_paint_every_world;
long flow_created_count(void) { return g_flows_created; }

/* The one constructor. Every flow is born here with a world already decided, so no flow exists without one and no
   caller can hand one a second. */
static Flow *flow_new(JSContext *ctx, JSValueConst fn, WorldId w) {
    g_flows_created++;
    if (g_flows_n >= g_flows_cap) {
        int nc = g_flows_cap ? g_flows_cap * 2 : 32;
        /* The registry is what the sale walks: this allocation can page a flow out (solver/reclaim.h), and the
           pager's flow_remove swap-removes from this array, so the array, its count and its capacity must describe
           the buffer that exists for the whole ask. The doubled capacity is published only after the realloc. */
        Flow **nf = reclaim_realloc(g_flows, (size_t)nc * sizeof(Flow *));
        CHECK(nf, "flow_add: OOM growing the frontier — a dropped flow corrupts BFS exploration");
        g_flows = nf; g_flows_cap = nc;
    }
    Flow *f = reclaim_calloc(1, sizeof(Flow));
    CHECK(f, "flow_add: OOM allocating a flow — a dropped flow corrupts the frontier");
    /* And it is born owing the host an image if the host asked for every world (@PERWORLD). Marked at the one
       constructor because every timeline passes it: a frontier walk names only the members standing at that
       instant, and an arm forked, run and ended between two host returns is never standing at one. At the mint and
       not at the fork: `flow_add_unseeded` has two kinds of caller (a from-baseline root and a branch's arm), and
       the question "was this world ever photographed" is asked of a world, decided here. */
    if (g_paint_every_world) flow_set_paint_owed(f);
    f->fn = JS_DupValue(ctx, fn);
    /* The pending register is empty, and empty is not an array. Most flows never park on anything, so an Array per
       flow would put a JSObject on the heap for every member of a frontier tens of thousands strong, and the
       blockedness question the preempt hook asks at every suspend point would read a property off it instead of
       testing a tag (on an empty Array it would read the register's count of the synchronous requests it is owed).
       Minted by the first push. */
    f->pending = JS_UNDEFINED;
    /* And the operation queue, empty for the same reason: a peer asks most documents nothing, and JS_UNDEFINED is
       not what calloc leaves behind. */
    f->perform_q = JS_UNDEFINED;
    /* And the delivery queue, the same: no peer posts to most documents. */
    f->deliver_q = JS_UNDEFINED;
    /* And which sending timelines this one is in, empty because a flow that has received nothing has committed to
       nothing: with no commitment recorded, every sender's world is still one this timeline may be in. */
    f->deliver_world_q = JS_UNDEFINED;
    /* And the job queue, the same: most flows are enqueued nothing. */
    f->jobs = JS_UNDEFINED;
    /* And the register's own context, named here because every flow passes this point; a line in each host's setup
       would be a hand-copied list. */
    pending_set_ctx(ctx);
    /* `cpu_gen` beside `cpu` because they are one quantity (flow_own_silence): the burn and the account generation
       naming its window. Zero matches the fresh account minted below, whose `emit_gen` is also zero, so an
       uncharged flow reads a current window holding nothing. */
    f->val = 0.0; f->cpu = 0; f->cpu_gen = 0; f->visits = 0;
    /* And the dispatch count, which nothing else writes to zero: a member arrives never-chosen and the number only
       rises. */
    f->picks = 0;
    /* And its place in the fork tree, minted here so no flow exists without a node its arms can be charged to. A
       from-baseline flow's node is a root (`up` NULL); flow_fork_inherit alone attaches one under another. */
    f->acct = reclaim_calloc(1, sizeof *f->acct);
    CHECK(f->acct, "flow_add: OOM allocating a flow's fork-tree node — a flow whose arms cannot be charged back "
                   "lets a fork chain burn the thread forever without its rank ever moving");
    f->acct->owner = f; f->acct->refcount = 1;
    /* And it opens a branch bucket of its own, true at this instant: a flow with no parent is a top-level arm.
       flow_fork_inherit alone moves it into another's, as a departure from this bucket and an arrival in that one,
       so neither counter falls. Depth zero likewise. */
    f->acct->branch = f->acct; f->acct->sub_born = 1;
    /* And the bucket's contribution to its members' weight, derived from the field above rather than written as the
       literal `1.0`, so acct_branch_offset's guard compares two evaluations of one expression on one operand, the
       one form in which an equality of doubles is exact. */
    f->acct->sub_offset = 1.0 / (double)f->acct->sub_born;
    f->acct->depth = 0;
    /* And it is its own family until something says otherwise: a from-baseline flow founds a family and keeps this;
       a fork joins its parent's (flow_fork_inherit) and this root becomes a plain ancestry node. Pointed here so no
       flow exists without an account for the aging term and the reward to read. The node arrives zeroed from
       `reclaim_calloc`, so a fresh family stands at reward 0 and silence 0, which the fork's and the arrival's
       preconditions check. */
    f->family = f->acct;
    g_acct_live++;
    f->cand_src = NULL; f->cand_payload = NULL; f->cand_sink = NULL;
    f->last_compiled = -1;   /* nothing compiled yet; see the no-replay DCHECK at the compile site */
    /* Nothing interposed yet; flow.h says why the pair is what says so and why -1 is a cursor no program can stand
       at. */
    f->imm_at = -1; f->imm_next = 0;
    f->world = w;
    /* The slot and the member are written as one statement, because they are one fact (flow.h's `reg_i`) and this
       is the one line the frontier gains a member on. Assigned here and not at the calloc: `reclaim_calloc` can
       page a flow out, and the pager departs through flow_remove's swap-remove, so `g_flows_n` may have fallen
       since the capacity check (it cannot have risen; only this line raises it). Read here, it names this flow's
       slot. */
    f->reg_i = g_flows_n;
    g_flows[g_flows_n++] = f;
    /* And the arrival counted as an arrival: this is the one place the frontier gains a member, so `g_arrivals` is
       a definition. The rank change below is also raised by other sites, so it cannot answer this. See the banner
       at `g_arrivals`. */
    g_arrivals++;
    frontier_rank_changed();   /* frontier changed: the newcomer may outrank the flow holding the thread */
    return f;
}

/* The arrival rule: start-time fair queueing's `max{v(t), F_prev}`, as a relation rather than a copy.
 *
 * flow_fork_inherit states the rule for a fork ("a continuation of an active flow enters at that flow's virtual
 * time, never at the system's, or any flow can reset its own virtual clock by splitting"). It is a rule about
 * entering, and this covers the other three entries, each of which founds a family (what "from-baseline" means)
 * whose account must be placed:
 *
 *   - engine_join_document's boot flow (engine.c): a Document the browser handed this agent mid-run,
 *   - solve.c's candidate session: one per @S search the run seeds, and
 *   - cold.c's park_flow_add: one per recipe the tier rebuilds.
 *
 * A flow at zero reward and zero silence carries only the optimism bonus, so it would enter at 1.0 against a
 * leader worth hundreds, below the entire backlog, for findings it had no opportunity to make. A copy of the
 * leader's reward fixes that for one instant only. In SFQ the server takes the minimum tag, so v(t) can never pass
 * a waiting member; this order takes the maximum and an emission raises the served account's tag, so v(t) runs
 * away from waiting members, and a coordinate stamped at creation is passed by what the incumbent earns afterwards.
 * Two newcomers created at two instants would then differ by something neither did.
 *
 * So the coordinate is read, not written, until the account is first served. An unplaced account's reward is
 * frontier_vt() (acct_family_val), re-read at every pick; the dispatch that first gives it the thread freezes that
 * reading into `base` (flow_set_running), and from then on it stands on its own tag plus its own ledger. That is
 * `max(v(t), F_prev)` evaluated continuously for a member with no F_prev, and it makes placement a fact about the
 * queue rather than a race with the newcomer's creation instant.
 *
 * It does not re-relate an account that has been served: such an account earns and ages on its own tag, so a
 * leader that keeps emitting leads and a served account that produces nothing can be passed. That is the bandit
 * ordering the scheduler asks for (accumulated emitted value, order only); whether a leader's advantage decays is
 * a different question. So `val_min` does not climb with `val_max`, and the alternatives are inadmissible:
 *   (a) re-relate until the account earns: an account that emitted once stands at its tag while one that never
 *       emitted stands at the clock, so a finding demotes its author by the whole reward spread, a promise
 *       outweighing a finding without bound, which flow_nonreward exists to prevent; and the never-earning account
 *       has been dispatched, so it is tried and unproductive, the arm optimism ranks lowest.
 *   (b) re-relate every account (`max(v(t), base + earned)` for all): every account below the clock flattens onto
 *       it, so `earned` orders nothing and the reward term is a common offset.
 *   (c) stop the clock running away: a reward that is a rate is forbidden ("Additive (not a literal value/cpu
 *       ratio)"), and a decaying leader advantage is a policy about the leader, not this door.
 * A uniform lift cancels, so the handicap is by construction a promise outweighing findings, and the principled
 * place to stop granting it is the account's first dispatch, the opportunity it compensates for. A pinned `val_min`
 * then says an account given its turn that produced nothing stands below one that produced. `val_unplaced` tells
 * that apart from accounts still held at the clock because nothing has reached them, and `turns` (solve.h) is
 * where the rule is visible.
 *
 * It cannot promote: the newcomer ties the frontier's virtual time on the queue coordinate and stands one optimism
 * range above it on the full weight (bounded at 1.0 by flow_nonreward), which it pays back at FLOW_AGE_QUANTUM per
 * quantum it burns. A page that manufactures documents and candidate sessions buys each of them turns and none a
 * monopoly.
 *
 * The function is an assertion, not an assignment. Nothing is copied: the optimism count reads 1.0 for a flow that
 * has completed nothing (flow_optimism), the reward is a reading, and the aging is not copied either. Copying the
 * incumbent's `flow_own_silence` and `acct_family_us` onto an account founding its own family would leave a debt
 * the source family forgives at its next emission (FlowAcct's `emit_gen`) but the newcomer can clear only by
 * emitting, which needs the dispatch the debt forecloses; and frontier_vt() is already the serving item's queue
 * coordinate, aging included.
 *
 * An account per branch arm is inadmissible for a separate reason. An arm founding its own account would arrive
 * here unplaced, copying nothing, but an unplaced account reads a clock, and `frontier_vt()` advances: two arms of
 * one parent founding accounts at two instants would arrive at two virtual times differing by what the frontier did
 * in between, the later one above (newest-first, the LIFO a copied reward produces). So a fork joins the family,
 * every member reads one reward, and "a never-run flow is never starved" holds within a family by construction. A
 * reader wanting the two sides of an example-free branch comparable is usually reading a throughput measurement as
 * an ordering one; the `neverPicked` block at the head of this file has the figures and their denominator. */
static void flow_arrive_at_virtual_time(Flow *f) {
    DCHECK(f->val == 0.0 && f->cpu == 0 && f->cpu_gen == 0 && f->visits == 0 &&
           f->family == f->acct && f->family->fam_us == 0 && f->family->emit_gen == 0 &&
           f->family->base == 0.0 && f->family->earned == 0.0 && !f->family->placed,
           "a from-baseline flow was charged, credited or DISPATCHED before it arrived — the arrival is the "
           "absence of every one of these, so a caller that wrote one first has ranked this flow at a virtual "
           "time nobody chose, and `placed` is in the list because a stamped account stops reading the clock "
           "at all and would stand for ever at a coordinate written before it existed on the frontier");
    /* The placement is the absence of a stamp, so there is nothing to write and nothing to skip when the frontier is
       idle: `frontier_vt()` answers SFQ's v(0) = 0 for a session that has dispatched nothing and holds the last
       serving coordinate between two slices. */
    DCHECK(!g_running || flow_is_member(g_running),
           "the flow holding the thread is not a member of the frontier, so the coordinate the clock is a "
           "reading of belongs to no queue — every unplaced account is being ranked against nothing");
    /* The rule, asserted where it is applied, as an equality against the clock rather than the incumbent. A
       newcomer enters at the frontier's virtual time, and flow_pick's strict comparison then leaves the thread with
       its holder, so a tie is what "arrives" means. It reads the clock and not `flow_queue_weight(g_running)`: the
       item in service sinks as it burns, and frontier_vt_serve does not follow it down, since a member given
       nothing may not be charged for the thread time of the one that has.
       Written over flow_queue_weight rather than a list of fields, for flow_fork_inherit's reason: a list cannot
       fire when a term is added, and a term a newcomer cannot stand at (anything read off its own payload or
       history) makes the two sides differ here, at the door. Two terms sit outside flow_queue_weight (see
       flow_optimism), so this is over the tags alone: the @S distance is a fraction of a payload the newcomer does
       not share, and the optimism bonus reads 1.0 for a flow that has completed nothing. Exact with no epsilon:
       the reward is frontier_vt() by construction (acct_family_val's unplaced arm) and the newcomer's aging notch
       is an integer division of zero, so both sides are the same float read twice. */
    DCHECK(flow_queue_weight(f) == frontier_vt(),
           "a flow arriving from the baseline did not enter at the frontier's virtual time — it was placed "
           "above the clock (a page promoting the documents and candidate sessions it creates over the whole "
           "backlog) or below it (a newcomer the ordering can never reach, which is what starved every @S "
           "candidate session behind a boot family whose account the whole frontier stood on)");
    /* And both halves of the aging are one reading, of nothing, which is what a member never offered the thread
       owes. Asserted at this transition like every other that can move either half. */
    DCHECK_AGING_ONE_WINDOW(f, "a newcomer entered at the frontier's virtual time");
}

/* The programs a new flow starts with, installed by the session and asked at the one place a flow is created. The
   scheduler does not know what a document's programs are (the engine's inventory), so the answer arrives as a hook,
   as the timer, rendering and checkpoint steps do. Asked here rather than at each creator, because a flow with no
   sequence runs nothing and finishes immediately, indistinguishable from a document whose scripts all completed.
   There are several creators of a fresh timeline of the root document (the boot flow, a cold-resumed flow replaying
   from its first script, an @S candidate session, and whatever is written next), and a seeding line at each is a
   hand-picked list whose missed member is silent. */
static void (*g_seed_hook)(Flow *f);
void flow_set_seed_hook(void (*fn)(Flow *f)) {
    DCHECK(fn == NULL || g_seed_hook == NULL,
           "a second component claimed the flow-seeding step — a new flow's program sequence has one source, "
           "and the second claim would silently replace the first for every flow created after it");
    g_seed_hook = fn;
}

/* A flow whose sequence its creator establishes itself: the fork (which inherits the parent's rows) and a joined
   document's boot flow (seeded with that document's programs, not the root's). The root sequence would make the
   fork leak the rows its copy then overwrites and the joined flow re-run the root's bundle in a second timeline.
   Named rather than defaulted, so the two exceptions say so. */
Flow *flow_add_unseeded(JSContext *ctx, JSValueConst fn, WorldId parent) {
    /* The world is minted here, so no flow exists without one. A fork passes the world it branched at and the child
       records the edge, which lets another instance materialize this flow's segment by forking the nearest ancestor
       it holds; a from-baseline flow passes WORLD_NONE and gets a root. `parent` is the fork point, not the other
       arm's world: the arm that keeps running is re-minted as a child of the same name (engine_sibling_assemble),
       so the two arms are siblings, which makes "do these two senders contradict?" answerable at a peer. */
    int from_baseline = world_is_none(parent);
    Flow *f = flow_new(ctx, fn, from_baseline ? world_mint() : world_mint_child(parent));
    /* And where it enters the queue, decided by the same predicate as where it enters the world tree, one question
       asked twice: a flow standing on another flow's decisions is a continuation and takes that flow's account
       (flow_fork_inherit), and a flow standing on nobody's arrives at the frontier's virtual time. Decided here
       rather than at the three from-baseline call sites. */
    if (from_baseline) flow_arrive_at_virtual_time(f);
    return f;
}

Flow *flow_add(JSContext *ctx, JSValueConst fn, WorldId parent) {
    Flow *f = flow_add_unseeded(ctx, fn, parent);
    /* The sequence is established before the flow is ever picked: a member picked with an empty sequence would
       report itself finished, the one answer that is never retried. */
    DCHECK(g_seed_hook != NULL,
           "a flow was added to the frontier with nothing to seed its program sequence — the session installs "
           "that hook when it opens, so this flow was created outside a session and would run no program at "
           "all and then report itself finished");
    g_seed_hook(f);
    return f;
}

/* Blocked: holding an unanswered synchronous host request. Counted, not scanned. The register is one flow's whole
   history of parks, not its outstanding requests: an entry stays until that flow delivers it, so the length only
   rises, and a per-opcode walk of it is the shape solver/pending_index.h records one level up. The drift hazard of
   a count beside the fact it summarises is answered at its level: the count lives on the register array, so the
   fork and the release carry it by construction, and it is audited against a full walk at those two sites, which
   already walk every entry. The answer comes from outside, so solver/pending.h has a door that is handed the
   register and a generic setter that crashes on that field. See pending.h at `pending_blocked` for why this
   classification can be counted when the reply-side ones cannot. */
int flow_blocked(const Flow *f) { return pending_blocked(f->pending); }

/* The delivery queue's length (flow.h). Same context and tag assert as the operation queue, because it is the same
   shape: an Array this engine appends to and nothing outside it touches. */
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

/* The unmade-delivery FIFO (flow.h has the shape, and why all three mutators live beside the field). An entry is an
   immutable two-element [record, senderOrigin] Array, so append, take-from-the-front and fork are the whole of it. */
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

/* Take the oldest, which is why this is a queue: HTML §9.3.3 "Posting messages" ends the window post message steps
   by queueing a global task on the posted message task source, and §8.1.7.1 "Definitions" gives a task a source to
   "group and serialize related tasks", so the page observes messages in arrival order. A swap-remove would hand the
   page one sender's second message before its first. */
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

/* One entry, without consuming it: the accessor both non-consuming readers use (engine_route compares the arriving
   record's world against every queued one; the cold tier writes them out), so the pair's shape is asserted once. */
JSValue flow_deliver_entry(const Flow *f, int i) {
    JSValue e;

    DCHECK(i >= 0 && i < flow_deliver_pending(f),
           "a flow's delivery queue was read past its end — the caller asked it for its length to get here");
    e = JS_GetPropertyUint32(pending_ctx(), f->deliver_q, (uint32_t)i);
    DCHECK(JS_IsObject(e), "a flow's delivery queue held something that is not a [record, senderOrigin] pair — "
                           "it is this engine's own Array and nothing outside flow_deliver_push appends");
    return e;
}

/* The arm's own queue: a new Array naming the parent's entries. The arm is that timeline continued, so a message
   that arrived before the branch arrived in the arm too, delivered under the arm's own delta: the multiplicity
   §7.2.1 has when a document's state is its flows, not one message delivered twice. The array is per-flow because
   each arm consumes at its own rate; the entries are shared because none changes. An empty queue forks as
   JS_UNDEFINED, and flow_deliver_push mints the Array if the arm is ever routed one. */
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

long flow_world_commit_rows_written(void) { return g_world_commit_rows; }

/* ─── Which sending timelines this one is in (flow.h's `deliver_world_q`) ───────────────────────────────
 * The same four operations as the queue above, over the same pair shape, and separate from it for flow.h's reason:
 * the queue is work this timeline still owes, this is what it has already become. An entry is never edited after
 * it is pushed, so there is nothing here but append, read, count and fork. */
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
    DCHECK(JS_IsObject(e), "a flow's delivery-world record held something that is not a [vector, taken, arm] "
                           "triple — it is this engine's own Array and nothing outside flow_world_commit_push "
                           "appends");
    return e;
}

/* Append one, inside cow_engine_write_begin/end for the queue's reason: this is the scheduler's record about a
   flow, written from outside any flow's delta, and a delta that captured it would un-commit a timeline the moment a
   sibling switched in. */
void flow_world_commit_push_at(JSContext *ctx, Flow *f, const char *vector, int taken, FlowCommitArm arm,
                               const char *file, int line) {
    JSValue e;

    /* The address is the abort's, and the abort is compiled out in release, so it is discarded here. It is not
       optional: the macro in flow.h supplies it at every call site, which is how this entry names its producer. */
    (void)file;
    (void)line;

    DCHECK(vector != NULL && *vector,
           "a receiving timeline recorded a commitment to no world — the record decides which of a sender's "
           "arms this timeline may still hear from, and an empty one constrains nothing at all");
    DCHECK(taken == 0 || taken == 1,
           "a delivery-world commitment carried something other than RECEIVED or FORECLOSED — the two are the "
           "whole vocabulary, and a third value would be read as one of them by whichever test asked first");
    /* Through flow.h's membership test rather than a list of names here: a second copy of the enum's set is the copy
       that drifts when a member is added. */
    DCHECK(flow_commit_arm_is_member(arm),
           "a delivery-world commitment named a minting mechanism this engine has no member for — the "
           "vocabulary is flow.h's FlowCommitArm and a producer states one of its members or states nothing "
           "true, so an unknown value here is either a producer that was never given a member or a park "
           "document written by something that is not this engine");
    /* And a foreclosed row's other side is its parent, by what the row means: "this flow is the arm minted where its
       parent took `vector`", so a foreclosed row naming no other side contradicts itself. Asked here rather than at
       the one live producer because the cold tier replays rows off a document, which can hand over a pair no live
       code makes. */
    DCHECK(taken != 0 || arm != FLOW_COMMIT_ARM_NONE,
           "a receiving timeline FORECLOSED a sender subtree while naming no flow on the other side of that "
           "branch — a FORECLOSED row means this flow is the arm minted where its PARENT took that world, so "
           "the other side is the flow this one was forked from and cannot be absent. A row like this refuses "
           "every record at or under its subtree and defers them to nobody");
    /* And the received rows stay pairwise consistent, the one claim this list makes that no row makes, asked at the
       append where a producer hands one over. Two received worlds that contradict are two sending timelines neither
       of which is the other continued, so no timeline of that document is in both (solver/engine.c's
       flow_answer_fork is about the asking side). This is not deliver_admits' gate, which asks whether an arriving
       record may be delivered: this asks whether the record itself is coherent, so it covers producers that do not
       exist yet. Foreclosed rows are skipped (the far side of a branch says nothing about what was heard), and
       WORLD_REL_INDEPENDENT is two forests, a different sending document or session, which every replayed park row
       is. world_vec_relate interns the names it parses, so it may not sit in a DCHECK condition (as
       solver/engine.c's engine_host_answer); nothing reads it in release, so the block goes with the check. The
       abort's triage is selected by the pusher's `file:line`. This retires when the received half holds one row
       per sending document, replaced in place as the timeline descends (deliver_commit_implied proves those rows
       are a chain whose deepest member decides every test). */
#if APICLIENT_DEV
    if (taken) {
        int n = flow_world_commits(f), k;

        for (k = 0; k < n; k++) {
            JSValue row = flow_world_commit_at(f, k);
            JSValue cv = JS_GetPropertyUint32(ctx, row, 0);
            JSValue tv = JS_GetPropertyUint32(ctx, row, 1);

            /* A foreclosed row is skipped structurally, by the branch, so the held vector and the arm it records
               stay in scope for the abort that prints them. */
            if (JS_VALUE_GET_TAG(tv) == JS_TAG_INT && JS_VALUE_GET_INT(tv)) {
                const char *held = JS_ToCString(ctx, cv);
                JSValue av;
                WorldRel rel;
                int held_arm;

                CHECK(held != NULL, "flow: OOM reading which sending timeline a receiving flow is already in — "
                                    "a commitment that cannot be read leaves the one pair this list can be "
                                    "wrong about unchecked");
                rel = world_vec_relate(vector, held);
                /* The held row's own producer, the half a threaded site cannot reach: the arriving row names its
                   pusher by the macro's address, while the held row was pushed at an earlier step and states only
                   the minting mechanism it wrote down, which is read here. */
                av = JS_GetPropertyUint32(ctx, row, 2);
                held_arm = (JS_VALUE_GET_TAG(av) == JS_TAG_INT) ? JS_VALUE_GET_INT(av) : -1;
                JS_FreeValue(ctx, av);
                DCHECKF(rel != WORLD_REL_CONTRADICT,
                        "a receiving timeline committed to TWO sending worlds that CONTRADICT — neither is "
                        "the other continued, so no timeline of the sending document is in both and this "
                        "flow's state now rests on a pair of peer timelines that never coexisted. PUSHED "
                        "FROM %s:%d. Arriving <%s>; already held <%s>, whose row states its own minting "
                        "mechanism as flow.h's FlowCommitArm %d (a -1 there is a row that carried no "
                        "integer in that field, which is a defect at whichever push wrote it and not this "
                        "pair). This row RECORDS a fabrication rather than causing one, so the repair is "
                        "the PRODUCER'S — and WHICH repair is what the address above decides, never this "
                        "line. solver/engine.c's answer_commit_taken: the cross-product an UNADDRESSED "
                        "cross-instance operation builds, engine_perform attaching one question to every "
                        "timeline the peer has. THE ADDRESSEE THAT CLOSES IT IS BUILT — remote_op.h's third "
                        "transport field, filled by engine_flow_addressee and consulted by engine_perform's "
                        "attach loop — so a row from there is no longer that gap and is one of two NARROWER "
                        "things, which is a repair rather than a queue. EITHER the asking flow addressed "
                        "NOBODY, which is a positive answer with three readings of its own: it had taken no "
                        "answer from that peer (correct, and then the pair above is two FIRST answers and "
                        "the arm for the second is flow_answer_fork's); or the record named a same-origin "
                        "CHILD of the peer agent, which no row matches because a world carries the minting "
                        "instance's ROOT document (engine.h's residual on engine_flow_addressee, whose next "
                        "diff is the row carrying the document it was answered FOR); or two rows for one "
                        "document were INDEPENDENT and the walk refused to guess, which is a park's "
                        "generation bump and is engine_perform's own residual. OR the peer answered from a "
                        "world INDEPENDENT of the addressee and the pin correctly refused nothing — the "
                        "same park case, arriving at the receiver. READ THE ARRIVING VECTOR'S GENERATION "
                        "AGAINST THE HELD ONE'S FIRST: equal generations is the first reading and means the "
                        "record went out unaddressed, and unequal is the park residual and means it went "
                        "out addressed and inert. solver/engine.c's deliver_commit_taken: "
                        "UNREACHABLE by construction, since deliver_admits refuses a contradicting record "
                        "and RETURNS before that push — so a row from there is a hole in THAT gate and is "
                        "a different and worse finding than this one. solver/cold.c's park replay: the "
                        "park DOCUMENT holds a pair no live push would have written, so it is a residue "
                        "from a build older than this check and the frontier re-derives rather than the "
                        "engine repairing it. Never widen this row to admit the pair",
                        file, line, vector, held, held_arm);
                JS_FreeCString(ctx, held);
            }
            JS_FreeValue(ctx, cv);
            JS_FreeValue(ctx, tv);
            JS_FreeValue(ctx, row);
        }
    }
#endif
    e = JS_NewArray(ctx);
    CHECK(!JS_IsException(e), "flow: OOM recording which sending timeline a receiving flow is in — without it "
                              "this timeline would go on to accept a message from the arm it did not take");
    cow_engine_write_begin();
    JS_SetPropertyUint32(ctx, e, 0, JS_NewString(ctx, vector));
    JS_SetPropertyUint32(ctx, e, 1, JS_NewInt32(ctx, taken));
    JS_SetPropertyUint32(ctx, e, 2, JS_NewInt32(ctx, (int)arm));
    if (!JS_IsObject(f->deliver_world_q)) {
        JS_FreeValue(ctx, f->deliver_world_q);
        f->deliver_world_q = JS_NewArray(ctx);
        CHECK(!JS_IsException(f->deliver_world_q), "flow: OOM allocating a flow's delivery-world record");
    }
    JS_SetPropertyUint32(ctx, f->deliver_world_q, (uint32_t)flow_world_commits(f), e);
    cow_engine_write_end();
    /* And the ledger's own count, raised on the line that performs the append. This is the only site that appends
       (the fork shares its parent's entries and states nothing), so a row on a flow that was never counted here is
       a producer that reached the Array another way, which solver/cold.c's preview asserts it cannot find. */
    g_world_commit_rows++;
}

/* The arm's own record: a new Array naming the parent's entries, to which the fork then adds the one entry that
   makes the two arms different timelines. The array is per-flow because each arm commits on its own from the
   branch; the entries are shared because none changes. An arm that lost its parent's commitments would re-accept
   a message its own timeline had already foreclosed. */
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

/* ───────────────────────────── The job queue ─────────────────────────────
 * flow.h says what a record is and why each part is there. Everything that happens to the queue is in this block,
 * so the Array's shape has one reader.
 *
 * The callee table. quickjs's job functions are statics of the interpreter, seen by the host only as pointers at
 * the enqueue hook, so a name for one is minted on first sight. Four exist (promise_reaction_job,
 * js_promise_resolve_thenable_job, js_dynamic_import_job, and host_call_job for every platform edge); the table
 * crashes rather than grows, because a fifth is a fact about the interpreter for a person to read. A code address is
 * stable for the process, so the table needs no teardown, and it is not reset with the frontier: an ordinal handed
 * out in one session must not name a different callee in the next while a record still names it. */
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

/* The record's layout, stated once. The header is fixed-width and the arguments follow it, so `argc` is a length
   subtraction, never a stored count that can disagree with what is there. */
enum { JOB_FN = 0, JOB_TASK, JOB_EXTERNAL, JOB_GLOBAL, JOB_HANDLE, JOB_SEQ, JOB_HDR };

/* The provenance bracket (flow.h). A static rather than a field because it is a property of the conversion in
   flight, not of the timeline: exactly one can be open, as the asserts say. */
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

/* One entry: the accessor every reader in this block uses, so the record's shape is asserted once. The caller owns
   the reference it gets back. */
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

/* A wide header field, read back: a second accessor rather than a widened job_field_int, because a header int must
   be a small integer, while a handle and an arrival stamp are integer values that leave the small-integer tag at
   2^31 and stay exact. Both tags are accepted and integrality is asserted, so a field that failed to round-trip
   crashes rather than answering a wrong number, the check js_task_handle_new's ceiling assert makes at the issuing
   end. It takes the consequence as a string: the shapes are one question and the consequences two (a handle that
   stopped round-tripping names another timeline's task; a stamp orders two items by a number that is neither's). */
static uint64_t job_field_u64(JSValueConst e, int field, const char *cost) {
    JSValue v = JS_GetPropertyUint32(pending_ctx(), e, (uint32_t)field);
    uint64_t h;

    if (JS_VALUE_GET_TAG(v) == JS_TAG_INT) {
        int n = JS_VALUE_GET_INT(v);

        DCHECKF(n >= 0, "a job record's wide header field is negative — both come from monotone counters that "
                        "start at 1, so this record was written by something other than flow_job_push (%s)",
                cost);
        h = (uint64_t)n;
    } else {
        double d;

        DCHECKF(JS_TAG_IS_FLOAT64(JS_VALUE_GET_TAG(v)),
                "a job record's wide header field is not a number — the header is written whole at the push, "
                "so one that is not there is a record something other than this file built (%s)", cost);
        d = JS_VALUE_GET_FLOAT64(v);
        DCHECKF(d >= 0 && d < 9007199254740992.0 && d == (double)(uint64_t)d,
                "a job record's wide header field is not an exact integer below 2^53 — the value is only a "
                "name while the number round-trips (%s)", cost);
        h = (uint64_t)d;
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

/* The queue's two kinds, counted in one walk; flow.h's `jobs_ready_task` says what the pair answers and
   solver/engine.c's task arm why the two are dispatched from opposite sides of one ladder. Both halves are counted
   and neither is subtracted: the reading is a partition, so its identity must be one two independent writers can
   fail, which `t + (n - t) == n` cannot. flow_wfq_census accumulates these and the queue's length separately and
   asserts the sum. It is a walk, so a caller asks it only of a member it has admitted: the census asks inside the
   ready arm alone, whose population `mem_unframed` bounds. */
void flow_job_kinds(const Flow *f, int *task_out, int *micro_out) {
    int n = flow_job_pending(f), i, t = 0, m = 0;

    DCHECK(task_out != NULL && micro_out != NULL,
           "a job-kind split was asked for with nowhere to put one of its two halves — the pair is the whole "
           "reading, and a caller holding one of them has a count with nothing to check it against");
    for (i = 0; i < n; i++) {
        JSValue e = job_entry(f, i);
        int task = job_field_int(e, JOB_TASK);

        JS_FreeValue(pending_ctx(), e);
        if (task) t++;
        else      m++;
    }
    *task_out = t;
    *micro_out = m;
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
    /* The key HTML §7.5.10 "Destroying documents" step 5 compares, held as a reference (flow.h). JS_GetGlobalObject
       is the realm's identity as a JS value, so a destroyed document's jobs are found by comparing two objects,
       neither of which can be freed underneath the comparison. */
    JS_SetPropertyUint32(ctx, e, JOB_GLOBAL, JS_GetGlobalObject(ctx));
    /* The name the runtime issued (flow.h's handle bullet says why it is a JS number). JS_NewInt64 keeps the
       small-integer tag for the first 2^31 handles and widens to a float64 past it; both are exact below 2^53,
       which js_task_handle_new asserts is the whole range. */
    JS_SetPropertyUint32(ctx, e, JOB_HANDLE, JS_NewInt64(ctx, (int64_t)handle));
    /* And when it arrived, on the clock the program sequence is named by (g_work_seq). This makes HTML §8.1.7.1
       "Definitions"' one-source-one-queue rule answerable for a flow whose work is split across two carriers: the
       sequence's rows carry the same clock in `dyn_id`, so flow_step compares two work items by when they were
       queued rather than which array holds them. Taken here, not at the runtime's enqueue: a handle must be minted by
       the runtime, whose own queues and baseline list issue callbacks this file never sees, while a stamp orders this
       flow's `jobs` against its `dyn`, the host's alone. The baseline handover therefore stamps a task when a flow
       adopts it, since nothing orders it against a row before it has a flow. */
    JS_SetPropertyUint32(ctx, e, JOB_SEQ, JS_NewInt64(ctx, (int64_t)flow_work_seq_next()));
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

/* Which entry the next pick would take, without taking it: shared by flow_job_take and flow_task_precedes, because
   the order is not a FIFO pop. Within a kind it is arrival order; across kinds a task may not begin while this
   flow holds a microtask (HTML §8.1.4.4 "Calling scripts"' clean up after running script step 3), so index 0 is
   right only when the queue holds no microtask. */
static int job_pick_index(const Flow *f, int n) {
    int pick = 0;

    while (pick < n) {
        JSValue c = job_entry(f, pick);
        int task = job_field_int(c, JOB_TASK);

        JS_FreeValue(pending_ctx(), c);
        if (!task) break;
        pick++;
    }
    if (pick == n) pick = 0;   /* nothing but tasks: the checkpoint is done, run the earliest task */
    return pick;
}

/* Does this flow hold a task queued before the row at its cursor? HTML §8.1.7.3 "Processing model" step 2.1's
 * choice of queue, made so that it cannot starve either side.
 *
 * Step 2.1 chooses a queue "in an implementation-defined manner" among those with a runnable task, and HTML
 * §8.1.7.1 "Definitions" bounds that freedom: a user agent may prefer a source "three-quarters of the time,
 * keeping the interface responsive but not starving other task queues". A fixed arm order cannot satisfy it: a
 * page's programs extend the program sequence (a lazy chunk, an injected `<script>`, a `javascript:` URL, a
 * response that is a program), and tasks run page code that appends rows while programs enqueue tasks, so either
 * strict order can exclude the other side for ever.
 *
 * Arrival order is free of that by construction: a work item's stamp never changes, so the items that outrank it
 * are fixed at its birth and finite, and whatever the page appends later is younger. Every item is reached with
 * nothing capped or counted down: an order, not a bound. It also discharges §8.1.7.1's ordering rule for a source
 * on both carriers (core/timing/task_source.h): within one source arrival order is queue order.
 *
 * The three refusals are each a rule of the standard:
 *   - `flow_stack_empty` is §8.1.4.4's "If the JavaScript execution context stack is now empty": a row at the
 *     cursor marked DYN_POS_IMMEDIATE is HTML §4.12.1.1 "Processing model"'s "immediately execute the script
 *     element el, even if other scripts are already executing", the synchronous tail of the program that queued
 *     it, so no task may begin in front of it.
 *   - an outstanding microtask is the checkpoint's, and the checkpoint arm stands above this ladder; answering yes
 *     would put a task in front of it and fire the ladder's own assert one arm down.
 *   - an empty queue has nothing to compare.
 * A caller with `seq_compiles` false has no row to compare against and must not ask. */
int flow_task_precedes(const Flow *f, uint64_t row_seq) {
    int n;
    JSValue e;
    uint64_t s;

    DCHECK(f != NULL, "the ladder asked which of two carriers holds the older work item of no flow");
    if (!flow_stack_empty(f)) return 0;
    n = flow_job_pending(f);
    if (n <= 0) return 0;
    if (flow_job_microtask(f)) return 0;
    e = job_entry(f, job_pick_index(f, n));
    s = job_field_u64(e, JOB_SEQ,
                      "a task would be ordered against this flow's program sequence by a number that is not "
                      "its arrival, so the page's own programs and its queued callbacks would run in an order "
                      "no clock states");
    JS_FreeValue(pending_ctx(), e);
    /* Strictly older, and there is no tie: one clock issues every stamp and never reuses one (g_work_seq), so two
       work items of one flow cannot share a number and `<` is total. */
    return s < row_seq;
}

JSValue flow_job_take(JSContext *ctx, Flow *f) {
    int n = flow_job_pending(f), pick, i;
    JSValue e;

    DCHECK(n > 0, "a job was taken from a flow whose queue is empty — the caller tested the queue to get here, "
                  "so the two reads have come apart");
    pick = job_pick_index(f, n);
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
    DCHECK(realm != NULL, "destroy a document step 5 was asked to remove the tasks of no realm at all");
    global = JS_GetGlobalObject(realm);
    cow_engine_write_begin();
    /* Compacted in one forward pass rather than spliced per hit; survivors keep their arrival order, which the task
       source guarantees the page. */
    for (i = 0; i < n; i++) {
        JSValue e = job_entry(f, i);
        JSValue g = JS_GetPropertyUint32(ctx, e, JOB_GLOBAL);
        int mine;

        DCHECK(JS_IsObject(g), "a job record carries no enqueuing realm — §7.5.10 step 5 keys on it, so a "
                               "record without one can neither be dropped with its document nor safely left "
                               "queued");
        mine = JS_VALUE_GET_PTR(g) == JS_VALUE_GET_PTR(global);

        JS_FreeValue(ctx, g);
        /* The one job this walk takes that nothing will re-cause, counted. HTML §7.5.10 "Destroying documents" step
           5 removes "any tasks whose document is document from any task queue (without running those tasks)", which
           is correct for a routed cross-document delivery too: its Document is gone, so no listener is left. But a
           routed delivery is the one job a replay does not re-cause (flow.h's record bullet on where the work came
           from, and cold.c's park assert), so, seen through `_routedDelivered` and the receiving page's own runs, a
           delivery removed silently is indistinguishable from one the scheduler lost. It is one of the four ends
           solver/engine.h declares, the same end core/frame/window_message.c reports when the task runs and finds
           the navigable destroyed. Naming it from the external bit alone is sound because flow.h's sentence makes
           the routed delivery the only work from outside the replayed program; a second kind would revisit this.
           Counted per arm: a fork gives each arm its own Array naming the same record, so one queued delivery is an
           end in each timeline holding a copy (hence the conservation law is an inequality), and this walk is over
           one flow's queue (solver/engine.c's drop hook says why). */
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

/* Remove one job by name (flow.h). The same forward-compacting pass as flow_job_drop_realm with the other
   predicate, not folded behind a mode flag, because the two questions differ in how many entries may match
   ("every job of this document" against "the one job called this"), which the assert states. The context is the
   queue's own: JSJobRemoveHook names a job by identity alone (a handle comes from one runtime counter and is not a
   fact about a document), so the caller has none to hand down. */
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

        /* Every entry is compared, including after a hit, because the count is what the assert is made of. */
        if (job_field_u64(e, JOB_HANDLE,
                          "a removal made with it would take some other timeline's task off the queue "
                          "instead of finding none") == (uint64_t)handle) {
            /* And this removal may not take a routed delivery. §7.5.10's walk removes the tasks of a document that
               is gone; this one is a tracker coalescing its own queued callback (HTML §4.11.4 "The dialog element",
               §4.11.1 "The details element") in a running document, so a routed delivery taken here would be a live
               page's message deleted by an element's toggle task. A delivery is pushed by the engine rather than the
               runtime's enqueue, so it carries JS_TASK_HANDLE_NONE and no tracker's handle names it; the handle
               field is all that separates the two, hence the assert. */
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

    /* The arm inherits the queue: an arm forked with a `.then` already attached must run it, or the arm reports a
       rejection unhandled whose `.catch` job never arrived. An empty queue forks as JS_UNDEFINED, and flow_job_push
       mints the Array if the arm is ever enqueued. */
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

/* The operation queue's length (flow.h). The context is the pending register's, for that file's reason: the
   callers that matter (the scheduler's pick, the teardown asserts) hold a Flow and nothing else. */
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

/* And whether it owes an answer at all (flow.h). Scanned rather than counted: both halves live in structures this
   file owns outright (the operation queue, whose length is an O(1) read, and the flow's token row, walked over
   `dyn_n`), so neither is the register the host settles from outside, which is what made a counter there
   necessary. Nothing has measured this as hot; if something does, the sites are all in this file. */
int flow_owes_answer(const Flow *f) {
    if (flow_perform_pending(f) > 0) return 1;
    for (int i = 0; i < f->dyn_n; i++)
        if (f->dyn_token[i]) return 1;
    return 0;
}

/* The service quantum: why the rank moves in steps rather than continuously. Aging applied continuously makes two
 * flows of equal value (the ordinary case: most have emitted the same amount, often none) trade places after every
 * step, a fair-share scheduler with an infinitesimal quantum that thrashes by construction, and here every trade
 * pays a full COW swap whose cost grows with the document. With elapsed time as the charge, the running flow would
 * fall below a tied rival between two consecutive consultations of the preempt hook.
 *
 * The rate is unchanged: a flow ages by exactly cpu * FLOW_AGE_RATE in the long run, so which flow wins over any
 * real interval is the same decision. What goes is sub-quantum resolution: rank moves one notch per quantum of
 * service, so the leader keeps the thread for a quantum and a tied rival then takes its turn. Nothing is dropped
 * or skipped; it is an order decision.
 *
 * The notch is the cooperative quantum, not a number of its own: ENGINE_QUANTUM_MS is the smallest slice this
 * scheduler hands anyone, and a private constant here would drift from it. An emission still preempts at once:
 * flow_credit_emit forgives the account's window and raises the generation, so the quantum bounds thrash between
 * equals, never the response to something that changed the ranking. */
#define FLOW_SERVICE_US ((int64_t)ENGINE_QUANTUM_MS * 1000)

/* The exchange rate between the two terms, and the mechanism: 1e-6 points per microsecond, one emitted finding per
 * second of thread time a flow holds without emitting.
 *
 * One second per finding prices silence on the timescale at which it monopolises and leaves a producing flow
 * alone: a flow that has emitted V findings and then stops falls below a never-run sibling (its own reward plus the
 * full optimism bonus, so at least 1.0 above the floor) after V + 1 seconds of silence. Nothing holds the thread for
 * a silent second and keeps its lead; nothing productive is demoted for the step in which it emits. flow_best
 * asserts that crossing. The silence is the chain's: a walk that forks a "stop at n" arm per position, each arm
 * running the rest of the document and departing, is charged through the family half (`fam_us`, FlowAcct) as each
 * arm burns, so the seconds counted are the seconds the chain spent. */
/* How many microseconds of silence one emitted finding is worth: the price. FLOW_AGE_RATE is its reciprocal, so
 * there is one place to change it.
 *
 * The price is not the unit. A term of an order has two independent quantities: what a microsecond costs, and the
 * smallest step the order can express. The price must be commensurate with an emitted finding; the step must be
 * commensurate with the granularity at which the thread is handed over, because a term that steps coarser than that
 * is invisible to every pick inside one step: the scheduler charges the flow, re-picks, and reads a weight the
 * charge did not move. With one constant as both, a step of one second is 83.3 quanta of silence, and the order
 * then rests on the optimism bonus alone, whose only mover is completing a unit (the same precondition as reaching
 * a queued job, `frame == NULL`), so the one flow that can run a job is the only one paying anything and is locked
 * out by deficits like 1/15 − 1/14 that a point-wide step cannot repay.
 *
 * The flow being charged accrues silence at twice its CPU (flow_age_running adds the same microseconds to `cpu` and
 * to the family's `fam_us`, and the notch is their sum), so a consuming flow pays two points per second where
 * FLOW_AGE_RATE says one; that is a pricing question, left as it stands. flow.h's weight
 * (`1/(1+v) - (s+F)*Q*RATE`, "the first quantum costs a candidate 0.012") and its census reasoning both assume a
 * notch of one quantum.
 *
 * The step may not cost more than an emission: at a step of one quantum the aging moves FLOW_AGE_QUANTUM points per
 * quantum, and above 1.0 a single slice would outweigh a finding. A relation between two constants, so asserted at
 * compile time. */
#define FLOW_SILENCE_US ((int64_t)1000000)
typedef char flow_quantum_costs_at_most_one_finding[(FLOW_SILENCE_US >= FLOW_SERVICE_US) ? 1 : -1];
#define FLOW_AGE_RATE (1.0 / (double)FLOW_SILENCE_US)
/* And the price of one notch, the price of one quantum. The aging term is `notch * this`, so the exchange rate is
 * untouched and the order can express the smallest amount of thread time it hands out. Floor then multiply, never
 * multiply then floor: two flows on the same notch get the identical product, which keeps the fork's
 * rank-neutrality equality exact. */
#define FLOW_AGE_QUANTUM ((double)FLOW_SERVICE_US * FLOW_AGE_RATE)

/* How many whole quanta of the thread this flow has consumed: the one quantised reading of `cpu`, a census quantity
 * saying who is consuming the thread at the granularity it is handed out in. Not a term of the weight; the
 * optimism term counts completed units (see flow_weight). */
int64_t flow_service_notch(const Flow *f) { return flow_own_silence(f) / FLOW_SERVICE_US; }

/* And the same reading of the fork family's: whole quanta the family has burned since any of its arms last emitted.
 * Two functions because they are two quantities with two reset points (flow_age_running), both the census's; their
 * ratio is the fork factor of the widest family, a fact about the document's branching. The aging term reads their
 * sum in the same unit, so a census row and a rank agree about how much service a member was charged for. */
int64_t flow_family_notch(const Flow *f) { return acct_family_us(f) / FLOW_SERVICE_US; }

/* How many whole cooperative quanta of silence this flow stands at: the aging term's quantity, the sum of the two
   notches' operands in their unit, the granularity at which the thread is handed out (FLOW_SILENCE_US says what a
   coarser step costs). The price is applied where the term is summed, at FLOW_AGE_QUANTUM, so this stays an exact
   integer and nothing quantises a rank on the FPU.
   Both halves are over one window, the family's silence epoch (FlowAcct's `emit_gen`), which lets them be summed
   into one notch and weighed against one reward. Each answers a comparison the other cannot: the family half
   orders one fork chain against another (without it a family that emitted V and wears N names pays for V once and
   presents it N times), and the own half orders a family's members against each other (without it every arm reads
   the same number, and on a frontier that is one family, as a real page's is, the term cancels out of every
   comparison). `int64_t`: at 12 ms a quantum this counts quanta of silence, so it cannot overflow anything a
   session reaches, and it is exact. */
int64_t flow_silence_notch(const Flow *f) {
    return (flow_own_silence(f) + acct_family_us(f)) / FLOW_SERVICE_US;
}

/* And the two pieces of that notch its published halves do not account for. The notch is `(own + fam) / S` and
   the rows beside it are `own / S` and `fam / S`, which do not sum to it: integer division of a sum is the sum of
   the divisions plus a carry, a whole notch (FLOW_AGE_QUANTUM, 0.012 points) against a whole-order spread of a few
   hundredths on real documents. Write `own = k*S + p` and `fam = K*S + R`, with `p` and `R` the residues and `S` =
   FLOW_SERVICE_US. Then, exactly for every non-negative pair,
        notch = k + K + (p + R >= S)
   and the three terms have three tempos:
     k  moves only when the flow being CHARGED crosses a quantum, and flow_age_running charges exactly one member,
        the running one, so between two dispatches this moves for AT MOST ONE MEMBER of the frontier.
     K  is the family's, read by every member through one pointer: a common offset that orders nothing within a
        family, and a real page's frontier is one family.
     the carry is one bit, and its threshold `S - R` is common and sweeps downward as the family burns, so members
        cross it in descending order of `p`: the boundary moving, not a member being re-keyed.
   So flow_pick's "N heap updates per dispatch" counts crossings, not updates: indexed by `p`, the sweep costs
   nothing per member and the maximum is the larger of two contiguous ranges of `p`. This pair is the key a
   sub-linear ask would be built on. It is not a term and not a second spelling of the notch: `-(double)notch *
   FLOW_AGE_QUANTUM` is one multiply of one exact integer, and an index summing three products would differ in the
   last bit and reorder members the order ties, so an index decides which members can be the maximum and the exact
   comparison stays flow_weight's. The identity is a theorem of integer division for non-negative operands
   (flow_age_running asserts the charge's sign), so it is not asserted; what can fail (nothing else in the weight
   moves between two frontier generations) is asserted in engine.c's preempt hook over its cached rival. This pair
   retires when the ask no longer walks the frontier and the index is its reader. */
int64_t flow_silence_phase(const Flow *f) { return flow_own_silence(f) % FLOW_SERVICE_US; }

/* And the bit itself, the one part of a non-running member's weight that moves with no generation bump behind it.
   `int`, not `int64_t`, because its only readers count members carrying it: a {0,1} quantity typed as 64 bits
   invites summing it into a notch. */
int flow_silence_carry(const Flow *f) {
    return (flow_silence_phase(f) + acct_family_us(f) % FLOW_SERVICE_US) >= FLOW_SERVICE_US;
}

/* Every term of the queue coordinate except the reward: the place in the queue, the tags an arrival must stand at.
   "The reward is the only unranged term" is the sentence every ordering assertion in this file is derived from, and
   this function is where it can be asked: as `flow_weight(f) − reward` it would be a subtraction of two composed
   doubles (cancelling badly once an account has accumulated) and a restatement of the formula at the asking site.
   As the same expression the weight is made of, the bound is asked of the quantity itself, and a term added here
   without a range is a fired assert. The fitness is not here: a per-payload reading is not a position a newcomer
   can be placed at (its payload is not the one the flow in service carries), so the arrival names it rather than
   copying it, which keeps flow_arrive_at_virtual_time's assert an exact equality. Nothing is copied at a fork or
   an arrival (the reward is read through a shared pointer or is frontier_vt(), and an arrival's notch is an integer
   division of zero), so two flows at one coordinate compare equal with no epsilon. */
static double flow_queue_nonreward(const Flow *f) {
    /* The aging: the flow's own silence and its family's. The family half makes a fork chain one accounting unit
       against every other family; it is blind inside one family, where every arm reads the same `fam_us`, and on a
       real page the whole frontier is one family, so the own half (this flow's burn over the same window) is what
       lets "a monopolizer that burns CPU without emitting sinks below productive+unrun flows" compare anything. Both
       are one quantity read at two scopes, with one reset (flow_credit_emit's window bump).
       It keeps the optimism term honest: a flow inside a program that never ends completes no unit, so its bonus
       never decays, and only this charge makes it sink and be passed by its siblings.
       It steps in cooperative quanta. A handover per quantum is start-time fair queueing rotating ("a deep loop
       suspends and yields so siblings run, then resumes"), not thrash, because an arm enters at its parent's
       virtual time (flow_fork_inherit), and it costs no extra switch: the cooperative quantum already returns the
       thread at this granularity. The price is FLOW_AGE_RATE, one finding per second; the step is 0.012 of a point.
       An emission is worth at most 1.0, not exactly 1.0: flow_credit_emit asserts only `v > 0.0`, and the @S
       filter-survival ratchet credits the increment between two fractions in [0,1]. So flow_nonreward's bound is
       asked of a quantity the reward never enters, exact because nothing in it is a difference of accumulated
       ledgers.
       Term by term: a flow that finishes its unit inside its quantum hands over on its visit (the optimism term's
       fairness); a flow inside a unit that never ends sinks 0.012 per quantum and its arms, which inherited less
       silence, pass it at the first quantum. O(1) to read, since every flow points straight at its family's root;
       the preempt hook's seam assertion snapshots this notch and the visit count (engine.c), between which a
       flow's weight moves only through an emission. */
    return -(double)flow_silence_notch(f) * FLOW_AGE_QUANTUM;
}

/* The optimism term, on its own, because it is a reading of the flow and not a place in the queue (the fitness
   distance's side of the split). "a UCB optimism bonus proportional to 1/(visits+1) so a NEVER-RUN FLOW IS NEVER
   STARVED": for a flow that has completed no unit it is 1.0, one emission's worth of promise, which
   flow_nonreward's bound prices. A visit is a trial: a flow preempted mid-program has not completed one, and a
   dispatch is the scheduler's decision, not a fact about the flow. A completed unit is the fact the reward
   depends on: a program that ends has reached its fetches, owes its checkpoint and can run its reactions.
   A newcomer cannot inherit it: an arrival stands on nobody's decisions (flow_add_unseeded), so it has finished
   nothing, and the incumbent's count would be a stranger's fact, differing between two newcomers by units neither
   completed. A fork carries it (flow_fork_inherit), because completed units are a fact about the executed prefix,
   which an arm is: two arms forked either side of a completed unit differ by it, the reading decreases in that
   count, and the older arm ranks higher, so the queue drains oldest-first. At the branch the arm ties with its
   parent and flow_pick's strict comparison leaves the thread there; the parent finishes its unit, its count
   advances, and every arm and never-run member then outranks it. So a flow monopolises one unit of work; a unit
   that never ends is the aging's job. Its range is unchanged: flow_nonreward adds it back and FLOW_NONREWARD_MAX
   prices it at zero visits. */
static double flow_optimism(const Flow *f) {
    return 1.0 / (1.0 + (double)f->visits);
}

/* The queue coordinate: the reward plus the remainder above, what an arrival is placed at
   (flow_arrive_at_virtual_time); every term is a tag a newcomer can stand at, not a reading of its own payload.
   The reward is the fork family's, not the flow's. The scheduler's reward is "accumulated emitted VALUE", and the
   accumulator is the account the aging is charged to: one emission raises it once, one second of that account's
   silence gives one point back, and the two terms are commensurate at one scope. Read off the flow it would be a
   per-chain prefix, two arms differing by what their parent held at two instants, unbounded against terms of range
   1.0, and on a one-family frontier the order would be birth position read newest-first. See FlowAcct's `val`.
   The reward has no range and needs none: every other term answers where an item stands now, while the reward is a
   ledger of what an account has produced, and bounding it would cap what a family may be worth. That asymmetry is
   why flow_nonreward's bound is one-sided. */
static double flow_queue_weight(const Flow *f) {
    return acct_family_val(f) + flow_queue_nonreward(f);
}

/* The one term of the order that is a reading of the item rather than a tag on the queue: outside flow_queue_weight
   (what an arrival is placed at) and inside flow_nonreward (what a range is asserted over). There is one comparator;
   every caller ranks by flow_weight.
   The @S fitness, read where a fitness is read: "The search is DISTANCE-DIRECTED (a fitness of {filter-survived,
   sink-reached, context-escaped, handler-fires} the WFQ reads)". It is a comparator, not a ledger: a ledger is paid
   at most once per observation, which makes every candidate that merely equals an earlier one worth what an
   unstarted flow is worth, so a search's later breakouts would run with the order blind to how far each got. Here
   nothing is paid or accumulated, and two flows at the same distance rank equal.
   Bounded by the optimism term's range, for the reason that term is bounded by one emission: a fitness says how
   promising an item is, not what it produced. At its maximum a candidate ranks with a never-run flow, enough to be
   picked out of a saturated frontier and not enough to hold the thread against a flow that emitted; it is aged
   like everything else. Zero for every non-candidate, since a flow with no payload has no ladder (both fitness
   writers assert it).
   It is the whole ladder: the survival rung alone is 1.0 for every candidate of a page that does not transform the
   payload, so the rungs are what tell those candidates apart (flow.h's `cand_rung` says why they live on the flow).
   With the runway fraction beneath them there are five rungs (FLOW_RUNGS_N) and the bands are [0, 0.2]
   undelivered, [0.4, 0.6] delivered, [0.6, 0.8] arrived, [0.8, 1.0] escaped. The denominator is the macro, not a
   literal, so a rung added beneath the others moves no arithmetic here or in flow_silence_us_to_sink; the rescale
   is order-preserving among candidates, since every candidate with a reading has the same rungs beneath it. It is
   held per flow and never accumulated, and the parked-candidate record ('c', cold.c) carries the substitution and
   not the ladder (a resumed candidate re-earns every rung by replaying to its source read), so a renumbering is not
   a format change unless a rung crosses the tier. */
double flow_distance(const Flow *f) {
    DCHECK(f != NULL, "the fitness term was asked of no flow — it is a reading OF an item, so there is no item "
                      "here for it to be a reading of");
    /* The pair invariant ("a flow with no payload stands at exactly 0") is asserted at engine.c's fork rather than
       here: it is transiently false by construction, because an arm of a candidate takes its parent's account
       (flow_fork_inherit) before its parent's substitution, and the rank-neutrality equality between those points
       calls this function. */
    return (f->cand_replay + f->cand_surv + (double)f->cand_rung) / (double)FLOW_RUNGS_N;
}

/* The bucket's contribution to the order, asked of the bucket: the one entry that reads the branch term without
   holding a member, which is what an ask that does not walk would need from this scope. flow_pick's banner says
   the branch term "is carried as a per-BUCKET OFFSET read at query time or the index is not sub-linear";
   flow_branch_bonus takes a Flow, so a candidate set could only bake it into a per-member key and pay O(live
   members of the bucket) per fork. Today its only caller is flow_branch_bonus, once per member.
   `branch_take` already enumerates the buckets, once per scan however many doors reach each (a per-node generation
   mark), with completeness asserted (`br_live_sum == members`) and the count published as `branches`. What is absent
   is an enumeration that does not walk the members, and before it the consumer: a per-bucket index fold at the
   pick, which does not exist, so a walk-free count alone would be a write with no reader. The residual at
   flow_index_key says what the walk costs.
   The guard is two writers against one reader: the stored double is maintained at flow_new and at
   flow_fork_inherit's join, and the quotient is recomputed here from the integer those statements wrote, the same
   expression on the same operand, so the equality is exact. It breaks only for a third writer of `sub_born` that
   leaves the offset, or a host with FLT_EVAL_METHOD 2 (evaluating wider than binary64 against a rounded store);
   both hosts this project builds for are binary64 throughout. solver/flow.h publishes `flow_branch_born` as the
   integer because a double "would be compared with `==` against a quotient recomputed at ANOTHER INSTANT" (engine.c's
   rival snapshot); this compares at one instant against the integer it was derived from. */
static double acct_branch_offset(const FlowAcct *br) {
    DCHECK(br != NULL,
           "a branch bucket's own contribution to the order was asked for with no bucket to ask — flow_new "
           "opens one for every node and the fork moves membership rather than clearing it, so a null here is "
           "an arm whose whole share of the order's fifth term is attributed to nobody");
    DCHECK(br->sub_born >= 1,
           "a branch bucket reports that it has never minted anybody while its contribution to the order is "
           "being read — `sub_born` is a LIFETIME count written to 1 at flow_new and raised at the fork, so a "
           "value below one is a retraction of a counter that may never fall, and the offset beside it was "
           "derived by dividing by exactly this");
    DCHECK(br->sub_offset == 1.0 / (double)br->sub_born,
           "a branch bucket's stored offset is no longer `1.0 / sub_born` — the two are written adjacently by "
           "flow_new and by flow_fork_inherit's join and are read together here, so a difference is a third "
           "writer of `sub_born` that left the offset standing, or a host evaluating doubles wider than "
           "binary64 and rounding at the store. Either way every live member of this bucket is about to be "
           "weighed on a denominator the order no longer holds, and ONE fork re-ranks a whole arm at once");
    return br->sub_offset;
}

/* The range of everything the order is made of except the reward, written as the derivation, term by term against
   flow_nonreward's summands (as wfq_accounted_spread is written against flow_weight's). Each line is one term at
   the end of its range that pushes the sum up:
     - the optimism bonus 1/(1+visits) decreases in a count that cannot go below zero, so its maximum is its reading
       at zero visits, the value a never-run flow carries and the unit one emission is priced against;
     - the fitness distance is (cand_replay + cand_surv + cand_rung)/FLOW_RUNGS_N with the two fractions asserted in
       [0,1] (flow_observe_replay, flow_observe_survival) and cand_rung on the ladder (flow_observe_rung), whose top
       is FLOW_RUNGS_N - 2, so its maximum is (1 + 1 + (N-2))/N = 1 for any N;
     - the branch bonus is 1/sub_born with `sub_born >= 1`, so its maximum is a member alone in its bucket;
     - the aging enters negatively at FLOW_AGE_QUANTUM per notch, and the notch is a floor of non-negative microsecond
       counts (flow_age_running asserts the charge), so it contributes at most nothing.
   One-sided by design: a flow that burns the thread without emitting must be able to sink arbitrarily far, and a
   floor would be a clamp ("a bound truncates distinct work"). What is bounded is how far a term may lift an item,
   because that decides whether a member can be put behind something other than a finding. */
#define FLOW_NONREWARD_MAX ( 1.0 / (1.0 + 0.0)                                          /* optimism, zero visits */ \
                           + (1.0 + 1.0 + (double)(FLOW_RUNGS_N - 2)) / (double)FLOW_RUNGS_N /* distance: two   */ \
                                                                                        /* fractions + the top  */ \
                                                                                        /* boolean rung         */ \
                           + 1.0 / 1.0                                                  /* branch: alone in it  */ \
                           - 0.0 * FLOW_AGE_QUANTUM )                                   /* aging, no silence    */

/* How many arms this member's branch has ever taken: the reading that tells the first arm of an unexplored branch
   from the many-thousandth arm of one already taken, which no other term can. On a real bundle whose frontier is one
   family (one reward for every member) every other term is common to the two sides of the branches that made the
   frontier, the state absent.c's banner names: "what must exist afterward is a frontier on which the two sides of an
   example-free branch are COMPARABLE AT ALL". This is a reading about the search, not about either arm.
   A bonus for a sparse bucket, not a penalty for a crowded one, and the sign is what makes it admissible: a crowding
   penalty would drop an unrun member beneath the 1.0 the crossing bound rests on ("an optimism term keyed on
   something a never-run flow does not stand at zero of"). As a bonus nothing is lowered, and its range joins
   FLOW_NONREWARD_MAX, loosening the silence bound by one point, the same shape as the fitness comparator.
   Fork-neutral by construction: a forked sibling joins its parent's bucket (raising `sub_born` before the fork's
   equality is evaluated), and this is read at the pick, so both arms read one number at every instant after the
   branch, which also passes the two-instants test. A root's own bucket is `sub_born == 1` for its life, asserted
   where the fork decides it.
   The denominator is the lifetime mint count, not the live gauge `sub_born - sub_gone`: occupancy falls when an arm
   departs, so a branch that minted a hundred arms and shed ninety-nine would read 1.0, like a branch that took none,
   a rank changed by branching through the departure, which the fork's equality cannot see. `sub_us` is inadmissible
   too: a root's own bucket accumulates every microsecond the root burns while an arm's fresh bucket stands at zero,
   so a term over it would make forking off a root a promotion, and seeding the arm with the parent's total is a copy
   at the fork instant (birth order). `depth` fails the same way (0 against 1). So the branch scope admits only the
   membership pair as a weight term, and its receipt rows stay observations.
   The cost to the guarantee: a never-run flow is still never starved (this only adds), but two never-run members
   now differ: one whose branch took no other arm outranks one of N by (1 - 1/N), at most a point. The range is
   unchanged, (0, 1], so FLOW_NONREWARD_MAX's `branch: alone in it` line and flow_silence_us_to_sink's `+ 2.0` stand.
   This record retires when no reading of a bucket's live count can be re-derived as a term: when `sub_gone` no
   longer exists, or the code makes an occupancy reading impossible. */
static double flow_branch_bonus(const Flow *f) {
    const FlowAcct *br;
    long live;

    DCHECK(f != NULL && f->acct != NULL && f->acct->branch != NULL,
           "a member was weighed with no branch bucket — flow_new opens one for every node and the fork moves "
           "membership rather than clearing it, so a null here is a member the branch census cannot reach "
           "either and its share of every per-branch reading is attributed to nobody");
    br = f->acct->branch;
    live = br->sub_born - br->sub_gone;
    DCHECK(live >= 1,
           "a member's own branch bucket reports fewer than one live member while that member is standing in "
           "it — `sub_born`/`sub_gone` are a matched pair moved in one statement, so a count below the member "
           "doing the reading is a departure charged to a bucket the flow had already left");
    /* And the denominator is the other half of that pair. The precondition above is the pair's own invariant and
       makes `sub_born >= 1` a consequence (`sub_born = live + sub_gone`, `live >= 1`, `sub_gone >= 0`). The return
       is the bucket's own reading: acct_branch_offset returns the double flow_new and the fork derived from that
       `sub_born` by the same expression, so flow_weight, flow_nonreward, flow_member_key and flow_index_surrogate
       read the bits a quotient here would give. This is the per-member adapter, the only thing the order calls. */
    return acct_branch_offset(br);
}

/* The denominator that term divides by (solver/flow.h says why it is published as the integer). Read through the
   same two indirections flow_branch_bonus reads, so the two agree on which bucket the member is in. The
   preconditions are asserted here because this is an entry from outside this file, whose callers have no other
   statement of them. */
long flow_branch_born(const Flow *f) {
    DCHECK(f != NULL && f->acct != NULL && f->acct->branch != NULL,
           "a member's branch bucket was asked for with no bucket to ask — flow_new opens one for every node "
           "and the fork moves membership rather than clearing it, so a null here is a member whose share of "
           "the order's fifth term is attributed to nobody and whose weight cannot be reconciled");
    DCHECK(f->acct->branch->sub_born >= 1,
           "a member's own branch bucket reports that it has never minted anybody while that member is "
           "standing in it — `sub_born` is a LIFETIME count written to 1 at flow_new and raised at the fork, "
           "so a value below one is a retraction of a counter that may never fall");
    return f->acct->branch->sub_born;
}

/* The half of the member key an index may be keyed on: every summand that is a reading of this member and of
   nothing it shares. The weight's terms partition by scope three ways: acct_family_val and the family notch are
   per account, flow_branch_bonus is per bucket (`1.0 / sub_born` through `f->acct->branch`, one pointer shared by
   every live arm, so the fork's `br->sub_born++` re-weighs the whole bucket and an index baking it into a member key
   "pays O(live members of the bucket) PER FORK"), and the own notch, the optimism and the fitness distance are per
   member (`f->cpu` against its family's `emit_gen`, `f->visits`, `f->cand_replay`/`cand_surv`/`cand_rung`), which no
   other member can move. flow_pick's banner names the same three groups.
   A subexpression, not a re-association: C evaluates `A + B + C + D` as `((A + B) + C) + D`, so flow_member_key is
   this value plus the bucket term, bit for bit, and a candidate set keyed here with the bucket offset applied at
   query time reproduces it exactly. solver/flow.h's FlowIndexChecks names the re-association that remains
   (flow_weight's own member half), an order change in the last bit and a decision. Excess precision (FLT_EVAL_METHOD
   2 rounding at the return) is the one way the identity fails, and flow_member_key's DCHECK guards it. Not a term:
   flow_weight is untouched.
   Named residual. Not covered: there is no candidate set, so flow_pick weighs every member and reads the bucket
   offset once per member. Three things stand between that and an ask that does not walk. Nothing enumerates the
   distinct buckets without walking members (`branch_take` reaches each through a member, and `branch_gen`
   de-duplicates within the walk). `FlowAcct` is private to this file, so a holder outside it uses
   `flow_branch_born`'s integer. And the family's `emit_gen` bulk-invalidates this key: an emission sends every
   member's `k` and phase to zero in one statement, and a real page's frontier is one family. That reset is uniform,
   which an epoch-keyed structure expresses with no per-member write; leaving it is bounded by dispatches plus forks
   since the last emission, which `FlowAcct.away_n` counts (summed into `g_epoch_rebuild` at each emission, published
   as `epochRebuildLifetime` against `epochResetsLifetime`, checked against a walk in flow_wfq_census). Its reading
   is `epochRebuildLifetime` over `scanNextWeights`, lifetime over lifetime and per host (testing/live-wfq.js
   composes it as `epochRebuildWalkShare`); it is not flow.h's `sil_phases`, which is a clock reading.
   Next diff builds: one commit holding a walk-free bucket enumeration (maintained at flow_new, the fork's join and
   acct_depart for a live-bearing count, plus acct_unref to mirror `branches` with empties) and the per-bucket fold
   over this key at the pick, with the walk kept inside a DCHECK as a cross-check. Read `branches` per document
   first: a single bucket makes the offset a common constant and the fold narrows nothing. A new branch-scope row
   lands in four files (flow.c, flow.h, result.c, testing/live-wfq.js, whose `branchScope()` reads result.c's
   `result_wfq_json` and throws on a `br`-prefixed row its lists do not name).
   Absence shows as: on consecutive @WFQ censuses of one run, the interval's `scanNextWeights` delta over its
   `scanNextRuns` delta, against `members` over the interval, reads about one (every member weighed per ask). Read
   per interval: lifetime counts against a terminal gauge under-read by the factor the frontier grew.
   This record retires when flow_member_key cannot be written without naming the scope each summand is read at. */
static double flow_index_key(const Flow *f) {
    return -(double)flow_service_notch(f) * FLOW_AGE_QUANTUM
           + flow_optimism(f) + flow_distance(f);
}

/* Every term of the weight that is not the family's: the quantity whose invariance between two frontier
   generations every proposed sub-linear order presupposes, and not the key an order may be keyed on, which is
   flow_index_key above. One quantity, two questions, and the stricter decides the composition: what must stand
   still between generations includes the bucket term, or flow_pick's walk goes blind to a fork re-ranking a whole
   arm; what an index is keyed on excludes it. Two functions over one composition.
   flow_weight is the family's coordinate plus this member's, minus a carry bit: the reward and the family notch are
   read through one pointer by every arm, a common offset that orders nothing within a family, and
   flow_silence_phase decomposes the carry out as one bit against a family threshold. Not a term and nothing ranks by
   it: a weight rebuilt as this plus the family's halves would sum three products where flow_weight sums one and
   differ in the last bit. It exists to be stamped and compared with itself, exact because it is the same
   expression on the same operands.
   The summands are in flow_nonreward's order, which arms the guard below: where the family has burned nothing since
   its last emission the two functions are the same operations on the same operands and agree bit for bit, a state
   every emission produces, so a summand reaching flow_nonreward and not this fires. Mentioned only inside DCHECK
   conditions, so release neither calls nor emits it, as acct_vt_leads and flow_is_min_weight. */
static double flow_member_key(const Flow *f) {
    /* The bucket term is the last summand, which makes the split exact: flow_index_key is this expression's first
       three summands, and this is that value plus the fourth. */
    double k = flow_index_key(f) + flow_branch_bonus(f);
    /* The flat spelling is the other side: two statements maintain the two, so a summand reaching one and not the
       other fires here, and so does a host evaluating doubles wider than binary64 that rounds at the call
       boundary. A new member-half term goes at the end of flow_index_key and a new bucket term after this one, the
       only placements under which both sides associate the same way. A second evaluation of the member half per
       call, so in a dev build flow_pick's per-member stamp pays this key twice; in release the condition is a
       `sizeof` and neither side is evaluated. */
    DCHECK(k == -(double)flow_service_notch(f) * FLOW_AGE_QUANTUM
                + flow_optimism(f) + flow_distance(f) + flow_branch_bonus(f),
           "the member half of the WFQ's weight is no longer its per-member half plus its bucket term — the "
           "two spellings associate identically by construction, so a difference is a summand that reached "
           "one of them and not the other, or a host evaluating doubles wider than binary64 and rounding at "
           "the return from flow_index_key. Every claim that a candidate set may be taken from the per-member "
           "key with the bucket applied as a query-time offset rests on this being EXACT rather than close, "
           "and an index built on a key that is merely close reorders two members this order ties");
    /* And the same equality as a holder of the published integer would take it, the one route across this file's
       boundary: the left side reaches the bucket term through `FlowAcct.sub_offset`, the double the order reads,
       and the right rebuilds it from `flow_branch_born`, the integer solver/flow.h publishes because `FlowAcct` is
       private. So the two halves an ask outside flow.c could hold, a per-member key and a per-bucket denominator,
       rebuild the member half bit for bit, which decides whether a candidate set may be taken from that key.
       Unlike acct_branch_offset's guard (a stored field against a quotient in one bucket), this compares two sums
       built by two routes, so it also catches excess precision rounding `k` at the store, and it is the one site
       where the integer an index would carry meets the double the order reads. Another evaluation per call in a
       dev build; nothing in release. */
    DCHECK(k == flow_index_key(f) + 1.0 / (double)flow_branch_born(f),
           "the member half of the WFQ's weight is no longer its per-member key plus `1/sub_born` over its "
           "branch bucket — the left side takes that term from the bucket's stored offset and the right side "
           "rebuilds it from the integer flow_branch_born publishes, and the two are written adjacently by "
           "flow_new and by the fork's join, so a difference is one of those statements having come apart or a "
           "host evaluating doubles wider than binary64. This is the exact equality every proposal for an ask "
           "that does not walk this frontier rests on: a candidate set keyed on the per-member half, with one "
           "offset applied per BUCKET at query time, reproduces this number or it has changed the answer");
    return k;
}

#if APICLIENT_DEV
/* What an index over that key would compare, written once so the check and its prose cannot drift: the member's own
   half shifted by the two quantities an index would hold per account. `acct_family_val` and `flow_family_notch` are
   read through one pointer by every arm, a common offset within a family; the carry is the one per-member bit that
   moves between two frontier generations, decomposed out by `flow_silence_phase`. That is
   `notch = k + K + (p + R >= S)` spelled as what a candidate set would be taken from. A re-association by design:
   flow_weight sums one product of one exact integer and this sums two, so it may differ in the last bit, and the
   fold in flow_pick measures whether that ever reaches the answer (solver/flow.h's FlowIndexChecks says what each
   outcome licenses). Dev-only, and it raises no scan counter: flow.h's FLOW_SCANS banner counts only the
   flow_weight the scan itself performs, and every caller of this is a DCHECK. */
static double flow_index_surrogate(const Flow *f) {
    return acct_family_val(f) + flow_member_key(f)
           - (double)(flow_family_notch(f) + flow_silence_carry(f)) * FLOW_AGE_QUANTUM;
}

/* How far that re-association may stand from the comparator, derived from the two expressions and never measured.
   The surrogate "may differ from the comparator in the last bit"; this bounds by how much, the claim an index can
   be built on. A candidate set of every member within this margin of the surrogate's extremum provably contains the
   comparator's extremum (the proof is at flow_pick's band walk), so re-comparing the survivors through flow_weight,
   with the same direction bit and tie-break, returns the full scan's member pointer for pointer, with flow_weight
   untouched. A tolerance fitted to the populations tried says nothing about the next frontier; this is an upper
   bound on |surrogate - weight| for every member of every frontier, and the fold's DCHECK asks it of every member
   weighed, so a wrong derivation fires.

   The derivation. Write `V = acct_family_val`, `P = N*Q` with `N = flow_silence_notch` and `Q = FLOW_AGE_QUANTUM`,
   `O = flow_optimism`, `D = flow_distance`, `B = flow_branch_bonus`; the exact target is `R = V - P + O + D + B`.
   The five leaves are the same calls on the same operands in both spellings, so their rounding cancels; only the
   arithmetic between them is bounded. `N = k + K + c` is an exact integer identity (flow_silence_phase), so the
   surrogate's target is the comparator's.
   Roundings: flow_weight computes one product (`N*Q`) and four sums (`+O`, `+D`, `+B`, `V+..`), five; the surrogate
   two products (`k*Q`, `(K+c)*Q`) and five sums (`+O`, `+D`, `+B`, `V+..`, `-..`), seven. The notch casts are
   exact (2^53 quanta is three million years of thread time) and negation is exact.
   Chains: let `L = |V| + P + O + D + B`; every exact intermediate of either spelling is a signed sub-sum of those
   five terms, so has magnitude at most `L`. Each rounding is `fl(a.b) = (a.b)(1+d)` with `|d| <= u = 2^-53`; along a
   left-associated chain whose fresh operand is exact `|e_j| <= |e_{j-1}|*(1+u) + u*L`, and each product contributes
   at most `u*L` at its leaf. So `|computed - R| <= n*u*L*(1+u)^n` for `n` roundings: `|weight - R| <= 5*u*L*(1+u)^5`,
   `|surrogate - R| <= 7*u*L*(1+u)^7`, and `|surrogate - weight| <= 12*u*L*(1+u)^7`.
   The constant is the next power of two, 12 -> 16, and the remaining `4*u*L` absorbs (a) the `(1+u)^7` factor,
   under 8e-16 relative; (b) up to three further roundings where FLT_EVAL_METHOD is 2 rounds at the returns from
   flow_index_key, flow_member_key and flow_nonreward (flow_member_key's guard catches that regime), taking the
   figure to `15*u*L`; (c) the rounding of `L` itself, three operations, so what is returned is at least
   `16*u*L*(1-4u)`, still above `15*u*L`; and (d) the single rounding of the band edge at flow_pick, at most `u*L`.
   `O`, `D` and `B` are bounded by FLOW_NONREWARD_MAX, whose value is the sum of the three positive terms' maxima
   (the aging summand is zero silence) and which flow_nonreward asserts; a term added to the order is also one more
   rounding in both spellings, which the fold's DCHECK reports.
   Not a term and nothing ranks by it: the member flow_pick returns is the comparator's extremum whether this is
   right or wrong. It is recomputed per member per ask from quantities a fork carries, and two arms may read
   different margins with no rank moving, since the margin decides which members are compared and flow_weight which
   wins. Dev-only, and it raises no scan counter, for flow_index_surrogate's reason. */
#define FLOW_UNIT_ROUNDOFF (1.0 / 9007199254740992.0)  /* 2^-53: binary64 round-to-nearest unit roundoff */
#define FLOW_INDEX_MARGIN_ULPS 16.0                    /* derived above: 12 roundings, next power of two up */
static double flow_index_margin(const Flow *f) {
    double v = acct_family_val(f);
    if (v < 0.0) v = -v;  /* negation of a double is exact, so the bound loses nothing to this */
    /* The scale factor is a power of two, so its product is exact, which is why clause (c) counts only the three
       operations of the sum and the one that scales it. */
    return (FLOW_INDEX_MARGIN_ULPS * FLOW_UNIT_ROUNDOFF)
           * (v + (double)flow_silence_notch(f) * FLOW_AGE_QUANTUM + FLOW_NONREWARD_MAX);
}
#endif

/* What the order is made of besides the reward, and the one place "the reward is the only unranged term" is asked.
   Every ordering assertion in this file is derived from that claim; asked against a reference member whose
   non-reward terms all stand at zero, it is asserted by nothing once that population empties (flow_pick says
   when), which is the stalled frontier the never-starved guarantee is about. Asked of one item, its population is
   every flow whose weight is read. It supersedes the pairwise form `w(best) − w(unrun) <= R(best) − R(unrun) + 1.0`:
   with N for this quantity that is `N(best) − N(u) <= 1.0`, and `N(u) = 1.0 + D_u >= 1.0`, so it says
   `N(best) <= 2.0 + D_u`, this bound weakened by `D_u` and gated on `u` existing.
   It catches an edit to the formula, not a state of the frontier: an optimism ceiling that stops being 1, a fitness
   that stops being a fraction of FLOW_RUNGS_N, an aging term allowed to go negative (a freshness bonus, a credit
   for waiting), and a summand added with no line in FLOW_NONREWARD_MAX, which fires on its first non-zero reading.
   Exact at its corner with no epsilon: the maximum is reached by a real member (a candidate at zero visits and zero
   silence whose bytes survived whole and escaped, alone in its bucket), and every term is exact in binary there
   (`1.0/(1.0+0.0)`, `(1 + 1 + (N-2))/N` as `N/N` for this small integral N, `1.0/1.0`, a zero notch times any
   price), so an epsilon would only be room for the next unranged term. The caller's address is not threaded: every
   route in is a request to rank a flow, and the remedy is always an edit to the terms summed below, here. */
static double flow_nonreward(const Flow *f) {
    /* Two readings and one tag: the optimism bonus and the fitness distance are facts about this flow, and the
       aging is the queue coordinate it shares with what it was placed beside. */
    double n = flow_queue_nonreward(f) + flow_optimism(f) + flow_distance(f) + flow_branch_bonus(f);
    DCHECK(n <= FLOW_NONREWARD_MAX,
           "a term of the WFQ's order other than the REWARD lifted a flow further than one emission and a full "
           "fitness reading can — so the ordering is no longer made of one unranged ledger plus terms that each "
           "span at most a finding, and §scheduler's 'a never-run flow is never starved' is a claim about a "
           "quantity nobody bounded. Bound the term in flow_queue_nonreward or flow_distance, or state its "
           "range in FLOW_NONREWARD_MAX beside the three that are already stated there");
    /* And the one state in which flow_member_key must reproduce this exactly: with the family's aging at zero and no
       carry, flow_queue_nonreward is `-(double)flow_service_notch(f) * FLOW_AGE_QUANTUM` and the three terms after
       it are the same calls in the same order, so a difference is a summand that reached one and not the other.
       Asked here because this is where a new term is added, so it fires on the diff that causes it.
       Its precondition is rarely true on a real page: `flow_family_notch(f) == 0` means the family has burned less
       than one cooperative quantum since any arm last emitted, and on a one-family frontier that empties the guard
       as it empties flow_pick's `unrun` population. So it is armed on the smoke fixture and just after an emission.
       A general guard would compare against a re-association, which differs in the last bit and would report a
       divergence the comparator does not have. The population is covered instead by flow_pick's walk, which stamps
       `key_last`/`phase_last` per member per scan and compares them across a frontier generation: that asks whether
       a value moved, this whether a summand reached one function and not the other. This record retires when
       flow_weight's member half is a subexpression of it rather than a re-association, so the equality holds for
       every family notch. */
    DCHECK(!(flow_family_notch(f) == 0 && flow_silence_carry(f) == 0) || flow_member_key(f) == n,
           "the member half of the WFQ's weight is no longer what flow_weight computes — with this family's "
           "aging at zero and no carry the two are the same expression on the same operands, so a difference "
           "is a summand that reached one of them and not the other. Every claim that a non-running member's "
           "rank cannot move between two frontier generations is derived from that pair agreeing, and the "
           "assertion in flow_best's walk would now be asking it of less than the order is made of");
    return n;
}

/* The comparator, written as the sentence every assertion in this file is derived from: the account's ledger plus
   everything else, so there is one place a new term can go that is not the reward, and it is bounded there. */
double flow_weight(const Flow *f) {
    return acct_family_val(f) + flow_nonreward(f);
}

/* The silence at which a flow is guaranteed to rank below any never-run sibling: the crossing the never-starved
   guarantee claims, computed from the flow's own reward rather than a fixed threshold that would fire falsely on a
   very productive flow. Compared against both halves of the silence, this flow's own and its family's: against the
   per-arm half alone it holds for a family whose arms each burn a sliver while the family monopolises the thread,
   and against the family half alone it holds for every member of a one-family frontier at once.
   Derived from flow_weight, and redone whenever the formula moves. An unrun candidate (all its terms at zero, as
   flow_pick's `unrun` tests) weighs its reward plus its distance and branch bonus plus 1.0, at least 1.0, so the
   pick's maximum is at least 1.0. For the picked flow, with `v` its visit count, `D` its fitness distance, `B` its
   branch bonus, `A` its silence notch in quanta and `Q` the price of one quantum (FLOW_AGE_QUANTUM):
   `val + D + B + 1/(1+v) − A*Q >= 1`, and `1/(1+v) <= 1` forces `A*Q <= val + D + B`. `D` and `B` are each in
   [0, 1] (FLOW_NONREWARD_MAX's derivation), so `A <= (val + 2) / Q`, and since `A` is a floor of
   `(own_silence + fam_us) / S` with `S` the cooperative quantum, `own_silence + fam_us < (val + 2) *
   FLOW_SILENCE_US + S`. The caller reads the own half through flow_own_silence, since the raw field is that
   quantity only while its window mark is current.
   Neither term is slack: each of the two extra points is the whole range of a term (the fitness comparator, the
   branch bonus) doing its job, and `+ S` is the width of the aging's own step, below which no bound can resolve.
   It catches an edit, not a state: a clamped or capped aging term, an optimism term without a ceiling of 1, a
   reward that grows with the CPU it is weighed against, a fitness comparator that is not a fraction, or an
   optimism term keyed on something a never-run flow does not stand at zero of, each the shape a fix for a
   thrashing scheduler takes. `static inline` so release, where the DCHECK's condition is unevaluated, does not
   warn. */
static inline int64_t flow_silence_us_to_sink(const Flow *f) {
    /* The reward is the one flow_queue_weight reads, the family's, the account the silence on the other side is
       charged to; read off the flow, the bound would be on a quantity the weight does not contain. `+ 2.0` is the
       fitness fraction and the branch bonus, each in [0, 1], per the derivation above. */
    return (int64_t)((acct_family_val(f) + 2.0) * (double)FLOW_SILENCE_US) + FLOW_SERVICE_US;
}

/* Host-owed marks (flow.h says what a mark means and why the flow carries one). A generation rather than a per-flow
 * flag, so clearing every mark is one increment rather than a walk of a frontier tens of thousands strong: a flow is
 * marked by stamping the current generation, and every stamp ages out when the generation moves. */
static unsigned g_owed_gen = 1;   /* NEVER 0: a fresh (calloc'd) flow must read as RUNNABLE, not as marked */

/* Published because it is flow_pick's own filter, the one place a member is kept out of the runnable order, and
   engine.c's yield decision asks the same question of the incumbent; a second spelling there would drift the day a
   second site learns to mark. */
int flow_host_owed(const Flow *f) { return f->owed_gen == g_owed_gen; }

int flow_host_owed_count(void) {
    int n = 0;
    for (int i = 0; i < g_flows_n; i++) if (flow_host_owed(g_flows[i])) n++;
    return n;
}

void flow_set_host_owed(Flow *f) {
    DCHECK(f != NULL, "a host-owed report arrived with no flow — the scheduler marks the flow it just stepped, "
                      "so this is a report about nobody, and the flow that made it is picked again immediately");
    /* A flow cannot report host-owed twice with no host event in between. A marked flow is out of the pick
       (flow_pick skips it as a candidate and refuses it as the seed), so it can be stepped again only if something
       cleared its mark, and the clears are the host's own events. A clear at the top of every slice would be wrong:
       a slice that ended because its CPU quantum expired hands the thread to a host with nothing to answer, so a
       frontier that is wholly blocked would be re-admitted before the sweep reached its end, and the stall would be
       unreachable while the engine swapped deltas without finishing anything. */
    DCHECK(!flow_host_owed(f),
           "a flow reported host-owed AGAIN with no host event in between — a marked flow is out of the pick, "
           "so it can only have been stepped because its mark was cleared by something that cannot have "
           "answered it. The frontier can then never be fully marked, the STALL is unreachable, and the "
           "scheduler re-asks members it has already asked for as long as the run lasts");
    f->owed_gen = g_owed_gen;
    /* And the ranking changed: the value yield fires "the moment a parked flow outranks", and the WFQ's answer is
       the best eligible flow, so the eligible set is a term of that answer. A mark takes a member out of the pick
       and a clear puts one back. The preempt hook caches its rival on this generation, and the value-yield
       assertion in engine.c's preempt_hook snapshots it to tell "the set changed" from "nothing changed". Raised
       through the existing generation, the one sentence that means the running flow's claim may have changed,
       rather than a second sentinel beside it. */
    frontier_rank_changed();
}

/* THE HOST ANSWERED THIS FLOW — see flow.h for why the clear is per flow and per EVENT. */
void flow_clear_host_owed(Flow *f) {
    DCHECK(f != NULL, "a host event cleared the mark of no flow at all");
    f->owed_gen = 0;   /* never equal to g_owed_gen, which starts at 1 and only ever moves forward */
    /* And this direction needs it as much: a clear makes eligible a flow the scheduler's last pick could not
       consider, and the pick and the preempt hook do not run at the same instant. A clear landing during the picked
       flow's first step (flow_deliver_one_reply settles the shared document-script slot for every flow waiting on
       one address) would hand the hook a rival the pick was never shown, with the generation, both notches and the
       reward unchanged: the pick and the hook answering one state two ways, which the value-yield assertion
       catches. */
    frontier_rank_changed();
}

void flow_clear_host_owed_all(void) {
    /* The wrap is handled: four billion clears is not impossible, and a stamp aliasing the new generation would read
       as owed on a runnable flow, excluding it until the next clear. Resetting the stamps costs one walk per wrap. */
    if (++g_owed_gen == 0) {
        for (int i = 0; i < g_flows_n; i++) g_flows[i]->owed_gen = 0;
        g_owed_gen = 1;
    }
    /* The whole-frontier form: every member just became eligible, the largest ranking change one call makes. The
       host reaches this between slices (engine_set_referenced and the provider's own edges), so the yield request is
       read at the first opcode of the next slice, when the new set first matters. */
    frontier_rank_changed();
}

/* The image a member owes the host (@PERWORLD); flow.h says what the mark means and why it is a flag where the
 * host-owed mark is a generation. Neither setter nor clearer raises `frontier_rank_changed`, unlike the pair above:
 * a host-owed mark takes a member out of the eligible set, so the WFQ's answer changes when one is laid or lifted,
 * while this mark is not in the eligible set, flow_weight or flow_pick's filter, and a member owing an image is
 * picked exactly when it would have been without one. Raising the generation here would tell the preempt hook and
 * the value-yield assertion that the ranking moved when it did not. */
void flow_set_paint_owed(Flow *f) {
    /* The mark is laid on two roads (this entry's call sites) and a NULL means a different thing on each, so the
       message names both. */
    DCHECK(f != NULL, "an image was asked of no flow at all — either paint_mark_standing_members, which "
                      "walks the frontier's own members, has read past its end, or flow_new has reached "
                      "the mint with no flow to mark, which its own allocation CHECK makes impossible");
    f->paint_owed = 1;
}

void flow_clear_paint_owed(Flow *f) {
    DCHECK(f != NULL, "an image ask was discharged against no flow at all");
    /* A discharge never asked for is the scheduler handing the host a turn nobody bought. Every caller is inside
       engine_sched_slice and clears the mark it has just tested, so an unmarked flow here is that test and this
       clear having come apart, at the cost of a yield per pick for the rest of the session. The two sites are the
       two closing edges of a turn: the FLOW_STEP_DONE arm, which buys a return so a member's last world can be
       rendered before flow_finish unapplies it, and the slice-end yield, which hands the host that member
       standing. A mark is spent by whichever comes first, so there is at most one image per ask. */
    DCHECK(f->paint_owed,
           "an image ask was discharged on a member that owed none — every discharge tests the mark and clears "
           "the one it tested, so this is a discharge reached by a route that never took the ask, and every "
           "later pick of this member hands the thread back for a picture no host asked for");
    f->paint_owed = 0;
}

void flow_paint_every_world(void) {
    g_paint_every_world = 1;
}

int flow_paint_owed(const Flow *f) {
    DCHECK(f != NULL, "whether a member owes the host an image was asked of no member");
    return f->paint_owed;
}

/* Which sub-quantum residues the frontier occupies: one byte per possible value of flow_silence_phase, stamped
   during the census walk and read at its end for `sil_phases`. The census's own scratch, never a term. An array
   over the key domain rather than a set over the members, because the domain is fixed at one cooperative quantum
   (12 KB) while the frontier grows, which is also the shape the index this row sizes would take. Cleared per census
   by `memset`, cheaper than a four-byte stamp per slot. The size is a compile-time relation: the domain is
   ENGINE_QUANTUM_MS * 1000, so a quantum in seconds would ask for a megabyte of static per instance. */
typedef char flow_phase_domain_is_affordable[(FLOW_SERVICE_US > 0 && FLOW_SERVICE_US <= 65536) ? 1 : -1];
static unsigned char g_phase_seen[(size_t)FLOW_SERVICE_US];

/* What asking the order costs; solver/flow.h's FLOW_SCANS says why the entries are counted apart, why the quantity
   is a count and not a clock, and why nothing may read these. Lifetime, never reset, and `long` because they are
   compared against `steps` and `forks`. */
static long g_scan_runs[FLOW_SCAN_N];
static int64_t g_scan_weights[FLOW_SCAN_N];   /* per member per scan — see FlowKeyChecks in flow.h */
/* And what the one assertion in that walk did with those weights; solver/flow.h states the partition, the kind, why
   the four are read through one call, and why all four at zero beside a nonzero scan row is a build that makes no
   check. Raised only in flow_pick's dev block; the storage and accessor are unconditional because solver/result.c
   publishes them in every build. */
static FlowKeyChecks g_key_checks;
FlowKeyChecks flow_key_checks(void) { return g_key_checks; }

static FlowIndexChecks g_index_checks;
FlowIndexChecks flow_index_checks(void) { return g_index_checks; }
#if APICLIENT_DEV
static int64_t key_checks_total(void) {
    return g_key_checks.armed + g_key_checks.stale_gen + g_key_checks.first_seen + g_key_checks.running;
}
#endif
long flow_scan_runs(FlowScan s) {
    DCHECK((unsigned)s < (unsigned)FLOW_SCAN_N,
           "an order-scan count was asked for an entry that is not in solver/flow.h's list — the index would "
           "read past the array, and the caller is about to publish whatever it found as a cost of this run");
    return g_scan_runs[s];
}
int64_t flow_scan_weights(FlowScan s) {
    DCHECK((unsigned)s < (unsigned)FLOW_SCAN_N,
           "an order-scan weight count was asked for an entry that is not in solver/flow.h's list — the index "
           "would read past the array, and the caller is about to publish whatever it found as a cost of this "
           "run");
    return g_scan_weights[s];
}

/* The three skips the order's walk makes, written once because two walks use them: the dispatch scan and the dev
   band walk, which must weigh exactly the population the scan weighed or its published count is over a different
   denominator. A pure predicate; `seed_live` is set by the caller first, so a pointer that is both the seed and the
   exclusion is marked live and then skipped, and the arms' order is unobservable because all three end in
   `continue`. */
static int flow_pick_skipped(const Flow *m, const Flow *seed, const Flow *exclude, int runnable_only) {
    return m == seed || m == exclude || (runnable_only && flow_host_owed(m));
}

/* The one order, asked through one scan. Four questions are put to the ranking (who should be running, who the
   running flow is defending against, this document's best weight for the host's Level-1 order, and which member the
   pager gives up first), and they are a seed, filters and a direction over one comparator, never separate rankings
   (one WFQ policy at both levels), so every pick is made under the assertions below.
   The incumbent is the seed, not a candidate like the rest: the scan compares strictly, so seeding it with the flow
   that holds the thread states the rule that the thread moves only for a strictly better flow, the rule the preempt
   hook's value clause applies at the other end. Without the seed the pick returns the first maximum in registry
   order, so a field of equal weights (the ordinary state) would hand the thread to g_flows[0] every iteration and
   pay a COW swap for an unchanged ranking. It is not a hysteresis margin, minimum service or switch budget: there
   is no number in it, and a strictly better flow takes the thread at the next opcode. */
static Flow *flow_pick(const Flow *seed, const Flow *exclude, int runnable_only, int worst, FlowScan why) {
    Flow *best = NULL; double bw = 0.0;
    /* The scan is counted here, not at its return, so every exit counts, including one added later. `why` is the
       caller's, threaded in rather than derived from the flags, since two entries share a flag shape (`flow_best`
       and the census read differ only in their question), as solver/step_unit.h's arm is assigned at each arm. A
       CHECK rather than a DCHECK, for solver/cold.c's reason at its histogram: the increment is a write in every
       build, so a dev-only guard would vanish exactly where the store happens. */
    CHECK((unsigned)why < (unsigned)FLOW_SCAN_N,
          "engine: the order was asked through a scan entry that is not in solver/flow.h's list — the value is "
          "only ever written from that enum at a flow_pick call site, so this is a cast or an uninitialised "
          "read, and the counters below would be written outside their array in every build");
    g_scan_runs[why]++;
    /* The best member carrying the full bonus and no aging: its weight is its reward + 1.0, at least 1.0, which is
       the property the assertions below need. The test is that premise, every non-reward term of flow_weight at
       zero read through the weight's own functions (the notch rather than its summands, so the population follows
       however many quantities the notch is built from). It is the best member rather than a boolean, because "a
       never-run flow is never starved" is a claim about a distance (how far the best untouched member stands
       behind the pick), as the census's `job_w_gap` is the reading `jobs_ready` cannot make.
       The population empties, which is why the range claim lives at flow_nonreward. `flow_silence_notch` reads the
       shared `fam_us`: once a family has burned one cooperative quantum since any arm last emitted,
       `acct_family_us` alone exceeds FLOW_SERVICE_US and no member of it answers this, and on a real page the
       frontier is one family. A fork does not refill it (the arm takes its parent's silence, window mark and
       family). An arrival does: a from-baseline flow copies nothing (flow_arrive_at_virtual_time), so it stands at
       zero silence and zero units, and every unplaced account keeps the population non-empty while it stands,
       which is when the never-starved guarantee is about somebody. flow_credit_emit refills it within a family
       for one quantum. The visit clause means what it reads, a member whose prefix has completed no unit, since an
       emission no longer zeroes the emitter's `visits`. A frontier whose accounts have all been served is the
       residual at the bottom of this function. */
    const Flow *unrun = NULL;   /* the best-weighted member with EVERY non-reward term of flow_weight at zero */
    double unrun_w = 0.0;
    /* And the best weight any never-dispatched member offers, a different population: `unrun` needs every
       non-reward term at zero, which flow_credit_emit's forgiveness gives to members that produced, while
       `picks == 0` is the population starvation is about and the one field nothing resets. Held here because the
       maximum is known only when the scan ends. */
    const Flow *never = NULL;
    double never_w = 0.0;
    DCHECK(!(seed && worst), "the eviction tail was asked with an incumbent to defend — the seed states who "
                             "keeps the THREAD, and the flow the pager gives up is not a question about that");
    /* Is the seed still standing: answered by the scan below rather than a second walk, which is why the
       eligibility block sits after the loop. This function visits every member and compares each against `seed`,
       by address against members the registry holds (flow_is_member's comparison), so a dangling seed matches
       nothing and is never dereferenced, and `g_scan_weights` prices the only walk the pick makes.
       Why this is still a walk: a heap keyed on flow_weight is invalidated faster than it can be kept. Every term
       except the aging moves only on an event that raises the generation or touches the running member; the aging
       is `-FLOW_AGE_QUANTUM * ((own_silence + fam_us) / FLOW_SERVICE_US)`, and `fam_us` is shared by the family,
       advances every step and raises no generation. The shared advance reorders nothing continuously; the floor's
       phase does, each member straddling its boundary at its own instant, once per FLOW_SERVICE_US of family time,
       which is one dispatch's slice: N heap updates per dispatch. flow_silence_phase makes that a count of
       crossings: the notch is exactly `k + K + (p + R >= S)`, so what moves with no bump is one bit against a
       threshold the family sweeps, a residue indexed by `p` with flow_weight untouched.
       The update cost is decided by scope, not by "moves only on an event that raises the generation", which is a
       cache's test (a bump drops a cached answer in O(1)); an index is maintained across the bump and pays for the
       keys the event moved. `acct_family_val` and the family notch are per account; `flow_optimism`,
       `flow_distance` and the own notch per member. `flow_branch_bonus` is neither: it is `1.0 / sub_born` through
       `f->acct->branch`, the fork's `br->sub_born++` raises it for the bucket the arm joins, and every live member
       of that bucket changes weight in that one statement (solver/engine.c's `g_rival_branch_born`: "one sibling
       forking re-ranks every member of the arm simultaneously"). So an index baking it into a per-member key pays
       O(live members of the bucket) per fork; it is carried as a per-bucket offset read at query time or the index
       is not sub-linear, and `br_fan_max`/`br_fan_sum` on the census price that choice. This record retires when
       the ask no longer walks the frontier, or when no per-bucket quantity can be summed into flow_weight. */
    int seed_live = 0;
    /* Whether the seed was a candidate at all, written at the one site that decides it: the fold admits the
       incumbent only if it is still registered and not host-owed on a runnable-only scan, and the plateau rows
       at the end ask "did the incumbent keep the thread" only of a seed the scan weighed. */
    int seed_weighed = 0;
#if APICLIENT_DEV
    /* The two sides of the identity that makes the arming count a measurement rather than a sum of its own
       summands, snapshotted here and compared where the loop ends (solver/flow.h's FlowKeyChecks). */
    int64_t kc_before = key_checks_total(), sw_before = g_scan_weights[why];
    /* And the member an index over the member key would have returned, folded beside the comparator's maximum over
       the same population with the same tie-break (solver/flow.h's FlowIndexChecks). */
    const Flow *sur_best = NULL; double sur_w = 0.0;
    /* And the widest margin any member of this scan carries, from which the band's edge is set: a maximum over the
       folded population, because the band must admit the comparator's extremum and the two members that argument
       concerns are known only when the fold ends. The band walk below proves the set exact. */
    double sur_mmax = 0.0;
#endif
    for (int i = 0; i < g_flows_n; i++) {
        double w;
        /* The registry handle, checked over every member of every scan (flow.h's `reg_i`), since the loop holds `i`
           and the pointer. Before the `continue`s, so a seeded, excluded or host-owed member is checked too: a member
           skipped from the order is not exempt from the registry's invariant. */
        DCHECK(g_flows[i]->reg_i == i,
               "a flow's registry handle names a slot the registry does not hold it at — the frontier gains a "
               "member on exactly one line and loses one on exactly one line, so the two writers of this "
               "relation have come apart, and flow_remove will swap-remove at the handle's slot and drop "
               "whichever member is standing there");
        /* The flag is set before the skip; the three skip arms are flow_pick_skipped's, shared with the band walk. */
        if (g_flows[i] == seed) seed_live = 1;
        /* Not a drop or a deprioritisation: a skipped flow keeps its weight, its place and its work items, and is
           picked again the moment anything could have answered it (flow_clear_host_owed). */
        if (flow_pick_skipped(g_flows[i], seed, exclude, runnable_only)) continue;
        /* Counted where the weight is taken, so the count is what the scan cost: the skip passes the seed, the
           excluded member and the host-owed ones unpriced, and `members` on the census says how big the frontier
           was. */
        w = flow_weight(g_flows[i]); g_scan_weights[why]++;
        /* And the precondition of every sub-linear order, asked of every member: engine.c's preempt hook holds it
           over the one rival it caches, and its retirement clause says an index "will hold it at its own update
           site rather than at one cached rival". It asserts flow_silence_phase's decomposition over the whole
           frontier: between two frontier generations only a non-running member's family half and carry bit may
           move, so its member half must stand still. Every writer of that half's quantities raises the generation:
           flow_observe_replay, flow_observe_survival and flow_observe_rung (distance), flow_credit_visit (optimism),
           flow_new and flow_remove (branch bucket), and flow_credit_emit through frontier_vt_serve (the emission
           that zeroes the own half). flow_age_running raises none and charges only the running flow, so the running
           flow is exempt; because the exemption is evaluated at the scan and `m == g_running` is false once
           flow_switch_out clears it, flow_age_running maintains the charged member's stamps itself, which is what
           the decomposition permits ("between two dispatches this moves for AT MOST ONE MEMBER of the frontier").
           One writer is correct by adjacency, hence an assert: flow_fork_inherit raises `sub_born` for the bucket
           the arm joins, re-ranking that arm, with no generation of its own, sound only because flow_add_unseeded
           bumped one just before and nothing weighs the frontier in between; the `sub_offset` written on the same
           statement moves with it or acct_branch_offset's guard fires. acct_depart's `sub_gone++` reaches no term
           (the branch term divides by the lifetime mint count), so a departure re-ranks nobody; its adjacency to
           flow_remove's bump matters to the census's membership identity (`br_fan_max <= br_live_max` in
           flow_wfq_census), not here.
           It raises no scan counter (flow.h's FLOW_SCANS counts only the scan's own flow_weight). Dev-only in check
           and stamp, for solver/flow.h's reason: a second evaluation of the member half and its remainder per member
           per scan is affordable in the build that checks and would change the run in the build that does not. */
#if APICLIENT_DEV
        {
            Flow *m = g_flows[i];
            double mk = flow_member_key(m);
            /* And the half of that key an index would be built on (solver/flow.h's `ikey_last` says why the sum
               standing still is not the claim a candidate set rests on). */
            double ik = flow_index_key(m);
            /* And the remainder that key throws away (solver/flow.h's `phase_last`). */
            int64_t mp = flow_silence_phase(m);
            /* Which arm of the disjunction this member took, decided once and read by both the count and the
               check, so the exemptions are not spelled twice; `kind != &g_key_checks.armed` is the predicate. The
               count scores this check's prediction that it never fires: a predicted absence is satisfied equally by
               an invariant that holds and a walk that compared nothing, and `armed` is the comparisons made (see
               solver/flow.h). The selector is `int64_t *`, the counters' width: it is the one writer of all four
               rows, and a narrower pointer type is a compile error rather than a silent truncation. */
            int64_t *kind = m == g_running   ? &g_key_checks.running
                       : !m->key_stamped     ? &g_key_checks.first_seen
                       : m->key_gen != g_gen ? &g_key_checks.stale_gen
                       :                       &g_key_checks.armed;
            (*kind)++;
            DCHECKF(kind != &g_key_checks.armed
                        || (mk == m->key_last && mp == m->phase_last && ik == m->ikey_last),
                    "a member that is not holding the thread changed its own half of the WFQ's weight with "
                    "the frontier generation standing still — flow_silence_phase's decomposition says only "
                    "the family's common half and the carry bit may move between two generations, and every "
                    "index, heap or cached maximum anybody builds over this frontier is derived from that. "
                    "Either a writer of the optimism, the fitness distance, the branch bucket or this "
                    "member's own silence has stopped raising the generation, or a weighing has been "
                    "introduced between one of those writes and its bump — flow_fork_inherit's `sub_born++`, "
                    "together with the `sub_offset` written on that same statement, is correct only by "
                    "adjacency and is the ONE writer here that is. acct_depart's `sub_gone++` is not among "
                    "them and this message used to say it was: the branch term divides by the LIFETIME mint "
                    "count, which no departure moves, so a departure re-ranks nobody and a fire here is never "
                    "about one. THE PHASE IS THE "
                    "SECOND OPERAND AND CAN FAIL WHERE THE KEY CANNOT: the key reads `own_silence / "
                    "FLOW_SERVICE_US` and the phase reads `own_silence %% FLOW_SERVICE_US`, so a write "
                    "smaller than one quantum moves the phase and leaves the key standing — and the phase is "
                    "what an index over this frontier BUCKETS on, so that write re-buckets a member with "
                    "nothing else in this engine able to say so. AND THE THIRD OPERAND IS THE HALF "
                    "WITHOUT THE BUCKET TERM, WHICH THE FIRST TWO CANNOT COVER BETWEEN THEM: the member "
                    "half is `flow_index_key + flow_branch_bonus`, so a fork's `sub_born++` moving the "
                    "bucket term DOWN while this member's own optimism, distance or service notch moves it "
                    "UP by the same amount leaves the sum standing and re-keys the member — and the key an "
                    "index is built on is the half, never the sum (flow_index_key's own banner: \"what an "
                    "index may be KEYED on is flow_index_key, the bucket term EXCLUDED\"). A fire on the "
                    "third operand alone is that compensation; a fire on the first and third together is an "
                    "ordinary member-half move and the bucket term is not involved. At generation %u the "
                    "member half was %.17g and is now %.17g; the phase was %lld and is now %lld; the index "
                    "key was %.17g and is now %.17g",
                    g_gen, m->key_last, mk, (long long)m->phase_last, (long long)mp,
                    m->ikey_last, ik);
            m->key_last = mk; m->key_gen = g_gen; m->key_stamped = 1; m->phase_last = mp;
            m->ikey_last = ik;
        }
#endif
        if (g_flows[i]->visits == 0 && flow_silence_notch(g_flows[i]) == 0 && (!unrun || w > unrun_w)) {
            unrun = g_flows[i]; unrun_w = w;
        }
        if (g_flows[i]->picks == 0 && (!never || w > never_w)) { never = g_flows[i]; never_w = w; }
        /* The tail is this scan read the other way, so the pager has no ranking of its own: the flow the cold tier
           gives up first must be the one the WFQ would run last, or the engine would page out a member the
           scheduler still wanted. One comparator, one scan, one direction bit. */
        if (!best || (worst ? w < bw : w > bw)) { best = g_flows[i]; bw = w; }
#if APICLIENT_DEV
        /* The same fold on the surrogate, with the comparison copied from the line above (direction, strictness,
           empty-accumulator arm), so the two name different members only when the surrogate disagrees. */
        {
            double s = flow_index_surrogate(g_flows[i]);
            /* And the derived bound asked of the member both spellings are in hand for, at no extra evaluation:
               `w` is the weight this loop took and `s` the surrogate it folded, so a derivation wrong about the
               roundings, the ranges from FLOW_NONREWARD_MAX or an added term fires on the member that refutes
               it. It is the precondition of the band below, whose proof is derived from this inequality holding
               for every member of the fold. */
            double mg = flow_index_margin(g_flows[i]);
            double dv = s - w; if (dv < 0.0) dv = -dv;
            DCHECKF(dv <= mg,
                    "the index surrogate stands further from the comparator than the bound derived from the "
                    "two expressions permits — flow_index_margin counts the roundings of each spelling and "
                    "scales the sum of the terms' magnitudes by them, so a fire is that derivation being "
                    "WRONG rather than the order being wrong: either a summand has been added to flow_weight "
                    "(which is one more rounding in both spellings and a wider range the margin's "
                    "FLOW_NONREWARD_MAX half does not know about), or a term's range is no longer what that "
                    "macro's summands say, or this host evaluates doubles wider than binary64 at more return "
                    "boundaries than the derivation's clause (b) allows. Every candidate set taken from this "
                    "key rests on this inequality and on nothing else, so this is the line to repair before "
                    "any of it. surrogate=%.17g weight=%.17g |difference|=%.17g margin=%.17g, over "
                    "val=%.17g notch=%lld optimism=%.17g distance=%.17g branch=%.17g",
                    s, w, dv, mg, acct_family_val(g_flows[i]),
                    (long long)flow_silence_notch(g_flows[i]), flow_optimism(g_flows[i]),
                    flow_distance(g_flows[i]), flow_branch_bonus(g_flows[i]));
            if (mg > sur_mmax) sur_mmax = mg;
            if (!sur_best || (worst ? s < sur_w : s > sur_w)) { sur_best = g_flows[i]; sur_w = s; }
        }
#endif
    }
#if APICLIENT_DEV
    /* The partition against the counter it partitions, both deltas in one hand. The four buckets are raised on the
       statement after `g_scan_weights[why]++` with no branch between, so over one loop their total moves by what
       that counter moved; two statements maintain them, so this is a check. Asked before the seed fold, which
       weighs one more member outside the stamped population (the seed is the incumbent the exemption is about).
       It catches a `continue` introduced between the weighing and the block, after which the arming count would
       describe a subset of the frontier the order walked. */
    DCHECKF(key_checks_total() - kc_before == g_scan_weights[why] - sw_before,
            "the member-key check ran on a different set of members than this scan weighed — the four buckets "
            "are raised on the statement after the scan's own weight counter with nothing between them, so "
            "over one loop the two deltas are the same number by adjacency. A difference is a branch that has "
            "been introduced between them, and `keyArmedLifetime` is then a count over a subset of the "
            "frontier the order walked while reading as a count over all of it. This scan weighed %lld "
            "member(s) and the check classified %lld",
            /* Cast, because int64_t is `long` natively and `long long` on wasm32, so no one specifier is right on
               both; this file's idiom for a 64-bit operand. */
            (long long)(g_scan_weights[why] - sw_before), (long long)(key_checks_total() - kc_before));
#endif
    /* The seed is folded in after the scan with a non-strict comparison, the same answer as seeding before the
       loop: there the maximum was over {seed} ∪ members with ties to the seed and then registry order; here the
       loop's maximum is over the members with ties to registry order and the seed takes it back on `>=`. Ties go
       to the incumbent ("a top-ranked flow runs on at ~zero switch cost"); a `>=` in the loop itself would hand the
       thread away at every opcode. `unrun` and `never` fold the same way. The direction bit is carried though
       `worst` cannot be set with a seed: that DCHECK is dev-only, and release must not answer differently on an
       argument pair nothing else rejects. The seed is weighed once here while the loop skips it, so
       `g_scan_weights` counts exactly the eligible members. */
    if (seed_live && !(runnable_only && flow_host_owed(seed))) {
        double w = flow_weight(seed); g_scan_weights[why]++;
        seed_weighed = 1;
        if (seed->visits == 0 && flow_silence_notch(seed) == 0 && (!unrun || w >= unrun_w)) {
            unrun = seed; unrun_w = w;
        }
        if (seed->picks == 0 && (!never || w >= never_w)) { never = seed; never_w = w; }
        if (!best || (worst ? w <= bw : w >= bw)) { best = (Flow *)seed; bw = w; }
#if APICLIENT_DEV
        /* And the seed takes the surrogate back on the same non-strict comparison, so a surrogate keeping the
           loop's winner on a tie does not name a different member by tie-break. */
        {
            double s = flow_index_surrogate(seed);
            /* And the derived bound is asked of the seed: the band's edge is set from the widest margin over the
               whole folded population, and leaving the seed out of `sur_mmax` could narrow the edge enough to
               exclude the member the candidate set is proved to contain. */
            double mg = flow_index_margin(seed);
            double dv = s - w; if (dv < 0.0) dv = -dv;
            DCHECKF(dv <= mg,
                    "the index surrogate stands further from the comparator than the derived bound permits, "
                    "at the incumbent — see the same check inside the scan loop for what a fire means and "
                    "what to repair. surrogate=%.17g weight=%.17g |difference|=%.17g margin=%.17g, over "
                    "val=%.17g notch=%lld optimism=%.17g distance=%.17g branch=%.17g",
                    s, w, dv, mg, acct_family_val(seed), (long long)flow_silence_notch(seed),
                    flow_optimism(seed), flow_distance(seed), flow_branch_bonus(seed));
            if (mg > sur_mmax) sur_mmax = mg;
            if (!sur_best || (worst ? s <= sur_w : s >= sur_w)) { sur_best = seed; sur_w = s; }
        }
#endif
    }
#if APICLIENT_DEV
    /* Whether an index would have answered what the comparator answered (solver/flow.h's FlowIndexChecks holds the
       readings). Asked after the seed fold, because it is a claim about the scan's answer and the seed is part of
       it. It counts and does not abort: the two spellings are declared free to differ by flow_index_margin, so an
       equality between their extrema is an expectation, not an invariant, and the band walk below carries the
       derived margin and is asserted. No epsilon is used anywhere.
       Two modes, stated by `sur_w` against the surrogate's reading of `best` (never against `bw`, which crosses both
       the spelling and the member): a tie (`sur_w == S(best)`), where the surrogate lost a distinction the
       comparator makes and the fold's tie-break (strict `>` in the loop, `>=` at the seed) named another member;
       and a strict disagreement, where the surrogate reads `best` strictly worse. The identity below makes them
       exhaustive. They are counted as `keyIndexDifferedTieLifetime` and `keyIndexDifferedStrictLifetime`, summing
       to `keyIndexDifferedLifetime`; no row separates within-account from cross-account disagreements.
       Disagreements arise from commensurability, not frontier size: FLOW_AGE_QUANTUM is 3/250, flow_branch_bonus
       `1/sub_born` and flow_optimism `1/(1+visits)`, so a member silent `d` quanta longer than a sibling whose bonus
       terms exceed it by `d * FLOW_AGE_QUANTUM` is tied in the reals (buckets of 75 and 750 give
       `1/75 - 1/750 = 3/250` exactly), and both spellings round a tie the order meant to have. Both approximate one
       real (`N = k + K + carry` is exact) and both round, so neither is the reference and a strict fire does not say
       which named the lesser member.
       The band walk answers both modes: its containment proof uses only that `b` carries the extremum. What a
       re-composed flow_weight would decide is tie identity, which members share the maximum, which on a frontier
       mostly tied at the top is most of what the order decides; that is the project owner's decision. This record
       retires when the order stops resolving below the step it can express (a tie predicate at FLOW_AGE_QUANTUM
       instead of `>` on a raw double, an order change), since there is then no sub-quantum verdict to disagree
       about. */
    if (best && sur_best) {
        g_index_checks.index_asked++;
        if (sur_best != best) g_index_checks.index_differed++;
        /* The surrogate's reading of the comparator's own member, the other half of the pair that states the mode.
           Taken once and used by the identity below and the mode count, one extra evaluation per scan; it raises
           no scan counter, for flow_index_surrogate's reason. */
        double sur_of_best = flow_index_surrogate(best);
        /* And the identity that makes the two modes the only two: `sur_w` is the surrogate's extremum over a
           population both folds admit `best` into on the same gate and direction bit, so the surrogate reads `best`
           at `sur_w` (a tie) or on the losing side (strict). Unlike the band walk's population check, which compares
           two walks' counts, this compares the extremum against a named member, so it also catches a fold that
           visited `best` and failed to admit it. */
        DCHECKF(worst ? sur_w <= sur_of_best : sur_w >= sur_of_best,
                "the surrogate's own extremum is not extremal over a population that contains the member the "
                "comparator returned — both folds admit `best` on the same gate and with the same direction "
                "bit, so this is an identity, and a fire is `best` having been weighed by the comparator's "
                "fold and dropped by the surrogate's. It also breaks what the PARTITION below rests on: with "
                "this false the surrogate's TIE and STRICT arms stop being the only two, and the two rows that "
                "count them are a partition of a total they no longer cover. sur_w=%.17g but the surrogate reads "
                "the comparator's own member at %.17g, over %d member(s) in direction worst=%d",
                sur_w, sur_of_best, g_flows_n, (int)worst);
        /* And the candidate set that answers both modes, walked so that its cost is a census row: every member
           within the derived margin of the surrogate's extremum, re-compared through flow_weight. flow_weight, the
           order and the member this function returns are untouched; the only claim is an equality asserted below.

           The set is exact. Write `S` for the surrogate, `W` the comparator, `M(f)` flow_index_margin (`|S(f) - W(f)|
           <= M(f)` for every member, asserted by the fold above), and `Mmax` the largest margin over the folded
           population. Let `b` be the comparator's pick and `m*` the surrogate's, so `S(m*)` is `sur_w`. Maximising:
           `W(b) >= W(m*)`, `W(m*) >= S(m*) - M(m*)` and `S(b) >= W(b) - M(b)`, so `S(b) >= sur_w - 2*Mmax` and `b`
           is in the band; the eviction direction reverses every inequality, hence the direction bit. The band is
           walked in registry order with the scan's strict comparison and the seed folded after with the non-strict
           one, so the comparator's tie-break is reproduced over a subset containing `b`: no band member can
           outweigh `b`, and none of equal weight can precede it. So `cand` is `best`, pointer for pointer.

           It is not a bound: it decides which member runs next, as the full scan does and by the same comparator.
           Every member keeps its weight, place, account and work items, and a member outside the band is weighed
           again at the next ask; the narrowing is of who is compared, with an answer proved identical.

           Two walks, because the band is defined against the surrogate's extremum, known only when the fold ends:
           a single pass counting against a running extremum overcounts by an order-dependent set (`0, w, 2w` at
           band width `w` counts three where the band holds two). The second walk costs one flow_index_surrogate per
           member weighed (instrument overhead; an index would hold its key) and one flow_weight per member admitted
           (what the design would pay per ask); dev-only, and no scan counter.

           `band` is how many members the set admitted and `band_of` how many the test was applied to, raised over
           the same walk, so their quotient is the share of the frontier an index would re-compare. Lifetime counts,
           summed over asks, never read against `members`. A small share is an index that narrows; most of the
           frontier is one that saves nothing, a finding for the project owner. `band_of` must move with the scan's
           own weight counter, maintained by another statement over another walk; flow_pick_skipped is shared so
           the two weigh one population, and the identity below fires if they stop. */
        {
            /* The edge carries the direction bit, as the seed fold does: `worst` beside a seed is refused only by a
               dev guard. `2.0 * sur_mmax` is exact and the subtraction rounds once, by at most `u` of its magnitude,
               clause (d) of flow_index_margin's derivation. */
            double edge = worst ? sur_w + 2.0 * sur_mmax : sur_w - 2.0 * sur_mmax;
            const Flow *cand = NULL; double cand_w = 0.0;
            long band = 0, band_of = 0;
            for (int bi = 0; bi < g_flows_n; bi++) {
                Flow *m = g_flows[bi];
                double s, mw;
                if (flow_pick_skipped(m, seed, exclude, runnable_only)) continue;
                band_of++;
                s = flow_index_surrogate(m);
                if (worst ? s > edge : s < edge) continue;
                band++;
                mw = flow_weight(m);
                if (!cand || (worst ? mw < cand_w : mw > cand_w)) { cand = m; cand_w = mw; }
            }
            /* The seed enters the band as it enters the scan: tested for membership and folded with the non-strict
               comparison, since the incumbent keeps the thread on a tie. It counts in the denominator whether or not
               the band admits it. */
            if (seed_live && !(runnable_only && flow_host_owed(seed))) {
                double s = flow_index_surrogate(seed);
                band_of++;
                if (!(worst ? s > edge : s < edge)) {
                    double mw = flow_weight(seed);
                    band++;
                    if (!cand || (worst ? mw <= cand_w : mw >= cand_w)) { cand = seed; cand_w = mw; }
                }
            }
            g_index_checks.band_members += band;
            g_index_checks.band_weighed += band_of;
            DCHECKF(band_of == g_scan_weights[why] - sw_before,
                    "the band walk and the order's own scan weighed different populations — the two share "
                    "flow_pick_skipped precisely so they cannot, and a difference is a `continue` introduced "
                    "into one of the two walks or a skip arm that reached one of them and not the other. "
                    "`keyIndexBandWeighedLifetime` is then a denominator over a set the order did not walk, "
                    "and the band share published against it is a fraction of the wrong thing. The scan "
                    "weighed %lld member(s) and the band walk tested %ld",
                    (long long)(g_scan_weights[why] - sw_before), band_of);
            DCHECKF(cand == best,
                    "a candidate set carrying the derived margin did not contain the member this comparator "
                    "returned, or contained it and did not return it — and those are the only two ways this "
                    "can fire. The first is the derivation at flow_index_margin being WRONG, and the fold "
                    "above asserts the inequality it rests on for every member, so a fire here with that one "
                    "silent means the band's EDGE is wrong rather than the margin: `2*Mmax` is the sum of "
                    "two members' margins and `sur_mmax` must be the maximum over the WHOLE folded "
                    "population, the seed included. The second is the TIE-BREAK: this walk visits the "
                    "registry in the scan's order with the scan's strictness and folds the seed afterwards "
                    "non-strictly, so a member of equal weight winning here and losing there is one of those "
                    "three having come apart. It is NOT the surrogate disagreeing with the comparator — that "
                    "is the TIE/STRICT partition below, which is a different claim and is COUNTED rather than "
                    "aborted on. The band held %ld of %ld member(s) "
                    "tested, edge=%.17g from sur_w=%.17g and 2*margin=%.17g; the band returned a member at "
                    "registry slot %d weighing %.17g and the scan returned slot %d weighing %.17g",
                    band, band_of, edge, sur_w, 2.0 * sur_mmax,
                    cand ? cand->reg_i : -1, cand ? cand_w : 0.0, best->reg_i, bw);
        }
        /* And which of the surrogate's two modes this scan was in, counted rather than asserted. An abort on
           `flow_weight(sur_best) == bw` would guard a state the declared margin permits on every real frontier,
           which is neither an invariant nor a missing capability; the design that answers it is the band walk
           above, which reads no mode and answers both arms. No gap bound is asserted in its place: the sound
           statement `0 <= bw - W(m*) <= M(best) + M(m*)` is entailed by the per-member fold (`|S(m) - W(m)| <=
           M(m)`) and the identity above (`W(m*) >= sur_w - M(m*) >= S(best) - M(m*) >= bw - M(best) - M(m*)`), so a
           third spelling could not fail first.
           The rows partition `index_differed` by the pair that states the mode. The sum is asserted because the
           total is raised where the members are compared and the arms here, over one condition.
           Named residual. Not covered: tie identity, which members share the maximum, which these rows count
           disagreements about rather than measure. Next diff builds: nothing in this file; an order that stops
           resolving below its step is an order change and the project owner's, decided from these rows. Absence
           shows as: a run whose strict arm is a large share of its differed total, the single top key naming a
           member the comparator calls worse repeatedly. */
        if (sur_best != best) {
            if (sur_w == sur_of_best) g_index_checks.differed_tie++;
            else                      g_index_checks.differed_strict++;
            DCHECKF(g_index_checks.differed_tie + g_index_checks.differed_strict
                        == g_index_checks.index_differed,
                    "the surrogate's two fire modes no longer partition the scans in which it named a "
                    "different member — the total is raised where the two members are compared and the arms "
                    "are raised where the mode is read, over what must be one condition, so a difference is "
                    "that condition having come apart in one of the two places and the share published "
                    "against the total is a fraction of a different population. It is NOT the surrogate "
                    "disagreeing with the comparator and it is not the band walk: those are the rows "
                    "themselves and the abort above. tie=%ld strict=%ld differed=%ld",
                    g_index_checks.differed_tie, g_index_checks.differed_strict,
                    g_index_checks.index_differed);
        }
    }
#endif
    /* "CPU-AGING so a monopolizer that burns CPU without emitting sinks below productive+unrun flows", asserted
       where the choice is made. It catches an edit to flow_weight, not a state of the frontier: the bound is
       derived from that function (flow_silence_us_to_sink), so it is an identity under the formula as it stands
       and fires when an edit breaks the derivation (a clamped or capped aging term, an optimism ceiling no longer
       1, a reward allowed to grow with the CPU it is weighed against), each the shape a fix for thrash takes. */
    DCHECK(worst || !best || !unrun ||
           flow_own_silence(best) + acct_family_us(best) < flow_silence_us_to_sink(best),  /* derivation there */
           "the WFQ re-picked a flow that has burned more thread time since its last emit — its own plus its "
           "fork chain's — than its entire accumulated reward is worth, while a never-run flow was waiting — "
           "the aging term is no longer commensurate with the reward it is subtracted from, so a monopolizer "
           "cannot sink");
    /* And the other half of the pair: "a UCB optimism bonus ∝ 1/(visits+1) so a NEVER-RUN FLOW IS NEVER STARVED".
       The bound above says a silent monopolizer sinks, not that the waiting flow is lifted, and the two can break
       separately. Also an identity derived from flow_weight: an untouched member weighs `val + 1.0` or more, and
       `best` is the maximum over the scan that set `unrun`. It is false once the optimism bonus is keyed on a
       quantity a never-run flow does not stand at zero of (thread time, say, since an arrival once stood at the
       running flow's service), given a range other than 1.0, or made to depend on anything the aging already
       prices; each lets "never starved" stop meaning anything while every other assertion passes. */
    DCHECK(worst || !best || !unrun || flow_weight(best) >= 1.0,
           "the WFQ picked a flow worth less than an untouched member of its own frontier — a never-run flow "
           "carries the whole optimism bonus and no aging, so nothing may be picked ahead of it below that "
           "floor; the optimism term is no longer the one §scheduler prices a never-starved flow against");
    /* The pairwise distance (`w(best) − w(unrun) <= R(best) − R(unrun) + 1.0`) is asked of one item at
       flow_nonreward, which needs no reference member. Its consequence remains: the inequality is tight in the
       reward and slack in everything else, so a frontier whose untouched members are unreachable has an
       unreachable reward gap, never an aging or optimism question. Members of one family read one reward, so within
       a family the gap is zero and the guarantee holds outright; a gap is between accounts (two documents, two
       searches, a resumed frontier's rebuilt recipes), and the census row that says which is `families` beside the
       reward spread, with `self_emit` saying whether the leading account earned its rank or was placed at it.

       Named residual. Not covered: the two guards above are gated on `unrun`, so they assert nothing on a frontier
       more than one cooperative quantum past its last emission in which every account has been placed (a long
       run's steady state once each arrival has had its first turn), nor on the picks inside that quantum at which
       the only member the silence clause admits has completed a unit. While an unplaced account stands it copies
       nothing and answers the test, so the guards are live. They are right where they fire and narrower than the
       sentence they cite: a claim about a pair needs a reference member whose weight is known without reading it,
       and the one such member is at zero silence, the population that empties. Next diff builds: a reference the
       frontier cannot lose, the weight a member would have at the family's own last emission, carried on FlowAcct
       beside `val` and `fam_us`, written once per family per emission and read by every arm. Absence shows as: a
       frontier whose census reports members at `turns:0` while `wTop` climbs, with every ordering assertion silent
       and nothing distinguishing it from a frontier served correctly. */
    /* Starvation, counted at the line that decides. The census's gauges (`never_picked`, `never_picked_gap`,
       `never_picked_at_top`) say a tied tail exists at some instant; none can say whether a pick passed over it.
       This counts the scheduler's own dispatch pick returning a member dispatched before while a never-dispatched
       member stood at the same weight, and only where the pick displaces the incumbent (`best != seed`): engine.c
       credits a dispatch inside `if (best != cur)`, so a scan returning the incumbent advances none, and counting
       retentions (nearly every scan of a frontier at one weight) would count the design as the defect. With the
       clause, this and `picksLifetime` count the same event and `starvedPicks <= picksLifetime` holds (result.c
       asserts it). Counted rather than asserted: one such pick is legitimate (the tie goes to the incumbent, and
       relaxing the strict comparison would hand the thread away at every opcode), and starvation is a rate.
       This row does not separate re-dispatches that continue a program (a framed flow finishing while an arm it
       forked, born at its weight, stands tied) from passes over a member with nothing in front of it.
       `g_starved_picks_idle` does: the subset whose member was between units (no live frame, no checkpoint owed,
       flow_between_units, and no parked continuation). The raise requires `best != seed`, so every member counted
       is one whose park queue is on `Flow::parked`, where flow_holds_park reads it exactly. Read the subset against
       `picksLifetime` and the remainder as what a forking frontier costs. The complement, the incumbent keeping the
       thread while a never-run member stands level, is the plateau rows below, raised under a separate condition;
       the rival scan, the host's best-weight read, the pager's tail and the census ask other questions.
       Exact `==`, no epsilon: two members at one weight read one value twice. Lifetime counters, off the flows for
       `g_picks_total`'s reason and reset by nothing, so they difference. Both raised under one condition, so
       `idle <= starved` holds by construction. */
    if (why == FLOW_SCAN_NEXT && best && best != seed && best->picks > 0 && never && never_w == bw) {
        g_starved_picks++;
        if (flow_between_units(best) && !flow_holds_park(best)) g_starved_picks_idle++;
    }
    /* And the complement, in one evaluation, so the identities hold by construction: `held` is decided by the same
       `best`, `seed`, `never` and `bw`, and every subset is raised inside its population's `if`, so
       `runs <= held <= asked` and `held_idle <= held` (asserted in result.c) fail only for a second writer.
       `never != seed` is part of the question: "a never-run member stood level with the incumbent" needs a member
       that is not the incumbent, and a seed with `picks == 0` would otherwise become its own `never` through the
       fold's `>=`. (That the incumbent has `picks > 0` is an argument about engine.c's displacement block, and
       `cur` is restored from `g_sess_cur` across a resume.) Closed by any dispatch scan that did not hold, asked
       or not, so the write to `g_plateau_open` is inside the scan test (see its banner). */
    if (why == FLOW_SCAN_NEXT) {
        int asked = seed_weighed && best && never && never != seed;
        int held  = asked && best == seed && never_w == bw;
        if (asked) g_plateau_asked++;
        if (held) {
            g_plateau_held++;
            if (!g_plateau_open) g_plateau_runs++;
            /* The one O(1) clause of the unit boundary, a field read rather than flow_between_units: this row
               fires on a retention, nearly every scan of a tied frontier, and flow_between_units' second clause
               (flow_job_microtask) walks the job queue taking and freeing a JSValue per entry, too costly on the
               hot path of a release build. `frame` is the boundary's primary clause, the one every job arm of
               flow_step is under; the row is an upper bound by the other two, as its residual says. */
            if (seed->frame == NULL) g_plateau_held_idle++;
        }
        g_plateau_open = held;
    }
    return best;
}

/* How much of an order spread the census's own rows can account for: one term of flow_weight per line, each at most
   its reported range, since the range of a sum is at most the sum of the ranges. The bound flow_wfq_census's closing
   assertion holds the observed spread to, written term by term so that a term added to the weight without a range
   here is visible in one place and a fired assert in the other. `val` and the fitness distance enter positively
   and contribute max−min (the distance's floor is 0, flow.h, so its maximum is its range). The optimism
   1/(1+visits) is decreasing, so its range is taken at the visit count's ends in that order. The aging enters
   negatively at FLOW_AGE_QUANTUM per notch, and flow_silence_notch floors the sum `(own_silence + fam_us)` while
   the census floors the halves separately: floor(a+b) <= floor(a)+floor(b)+1, so the two spreads plus one notch
   bound it exactly, the only slack here. */
static double wfq_accounted_spread(const WfqCensus *c) {
    return (c->val_max - c->val_min)
         + c->dist_max
         + (1.0 / (double)(1 + c->vis_min) - 1.0 / (double)(1 + c->vis_max))
         /* And the branch bonus's range, the least-minting bucket against the most (the extrema invert, since the
            fewest arms carry the largest bonus). The mint pair, not the live pair, because flow_branch_bonus
            divides by `sub_born`, and the live pair bounds the mint range in neither direction: two buckets at one
            live member each span 0.0 live and 1 - 1/N here when one has minted N and shed them. Guarded because a
            census that reached no live bucket has no range to report; branch_take takes both mint extrema inside
            its live guard, so the two ends are over one population. */
         + (c->br_born_min > 0 && c->br_born_max > 0
              ? 1.0 / (double)c->br_born_min - 1.0 / (double)c->br_born_max : 0.0)
         + (double)((c->svc_max - c->svc_min) + (c->svc_fam_max - c->svc_fam_min) + 1) * FLOW_AGE_QUANTUM;
}

/* The mark the census counts families with (FlowAcct's `census_gen`): a generation rather than a clear pass, for
   the host-owed marks' reason. */
static unsigned g_wfq_census_gen;

/* And the bucket the live maximum is owned by, retained across the walk so its own three numbers are read from one
   node rather than assembled from three extrema (flow.h's `br_crowd_live` says what that buys). A file static, not
   a census field, because `FlowAcct` is private here and the census struct is the public seam. It shares the
   generation mark's lifetime: set by branch_take and cleared by flow_wfq_census at both ends of one scan, during
   which nothing steps the engine, so no node is minted or freed while it is held. Cleared after it is read as well
   as before the walk, because between censuses the next departure may free the node it points at. */
static FlowAcct *g_wfq_census_crowd;

/* And the bucket the mint maximum is owned by, retained for the same reasons and under the same lifetime and
   clearing rules. A second pointer, not the crowd under another name: the crowd is selected by the live maximum and
   this by the mint maximum, and `sub_born = live + sub_gone` makes those one bucket only while nothing has been
   shed, so an arm that minted unboundedly and let each arm finish owns this maximum and not that one. flow.h
   states what the triple buys and why `br_born_max` alone cannot. */
static FlowAcct *g_wfq_census_minter;

/* Open one branch bucket into the census, once per scan however many doors reach it: the mark is the node's own, so
   this is idempotent within a scan and the sums are a partition. A function rather than a macro because nothing
   here can fail. Called with a node the caller has asserted is a bucket (`branch == self`). */
static void branch_take(WfqCensus *out, FlowAcct *br) {
    long live;
    if (br->branch_gen == g_wfq_census_gen) return;
    br->branch_gen = g_wfq_census_gen;
    live = br->sub_born - br->sub_gone;
    out->branches++;
    out->br_live_sum += live;
    out->br_us_sum += br->sub_us;
    /* The live extrema range over buckets that hold a member, the burn totals over every bucket taken: two
       populations through one walk. A bucket with zero live members is the ordinary state once a family's root
       flow has departed: the family-root door takes exactly such a node, deliberately, because its lifetime burn
       (boot's, on a real page) must stay inside the identity at the end of this function and nothing names it
       through `branch` any more. A minimum over an empty bucket describes the thinnest bucket nobody stands in, and
       would score the branch bonus's whole range as 0.0 in wfq_accounted_spread. `br_us_max` and `br_us_min` keep
       every taken bucket: a departed root's subtree really did receive that thread time, and the burn identity is
       over the same population. `br_live_min == 0` means "no live bucket seen yet", since every assignment below
       writes at least one; flow_wfq_census's closing assert says a frontier with members cannot leave it at zero.
       The fixture and a real document read oppositely on this scope (a few percent of members in the most crowded
       of many buckets, against one bucket holding more than half of a one-family frontier), so a fixture reading is
       a hypothesis about real pages. */
    if (live > 0) {
        /* The held half of the burn split, with its twin in the arm below, so the two are raised by two
           accumulators under one condition rather than by one and a subtraction. The crowd triple's receipt is drawn
           from live buckets, so its honest denominator is the live buckets' receipt, not `br_us_sum`, which keeps a
           departed root's whole burn (the largest single term on a real page). Two rows, so
           `br_held_us + br_empty_us == br_us_sum` checks this guard. */
        out->br_held_us += br->sub_us;
        if (out->br_live_min == 0 || live < out->br_live_min) out->br_live_min = live;
        if (live > out->br_live_max) {
            out->br_live_max = live;
            /* And the bucket itself, retained rather than copied, so `br_crowd_live == br_live_max` compares a
               second reading of the bucket's pair (dereferenced once, after the walk) against this running maximum,
               rather than an assignment against itself. The membership pair is the selector, not the burn: the row
               is the crowd's receipt, and the hungriest bucket is already `br_us_max`. */
            g_wfq_census_crowd = br;
        }
        /* …AND THE MINT PAIR, OVER THE SAME POPULATION AND INSIDE THE SAME GUARD, BECAUSE IT IS THE ORDER'S
           RANGE AND NOT A RECEIPT. `sub_born` is what flow_branch_bonus divides by, so these two extrema ARE
           the branch term's range and wfq_accounted_spread reads them as such. A member's weight is only ever
           read for a member that is STANDING, and a member stands only in a bucket with a live count — so an
           empty bucket contributes no weight to any comparison, and extrema taken over one describe a range
           nobody spans. That is `br_live_min`'s own incident, and the reason it is repeated here rather than
           cited is that the two pairs are folded by two statements and only a guard keeps them one population.
           `br_born_max` MOVED INSIDE THIS GUARD IN THE DIFF THAT ADDED `br_born_min`, and that is a narrowing
           of a published row rather than a new row's placement. It used to fold every bucket the walk took,
           which made it disagree with its own declaration in flow.h ("the most members ever MINTED into one
           live bucket") and with `br_live_max`, the row flow.h pairs it with. The `br_us_*` pair below
           deliberately keeps EVERY taken bucket and that is not an inconsistency: RECEIPT is the question
           those ask, a departed subtree really did receive its thread time, and the burn identity is over the
           same population. Membership is not receipt, and a maximum that can be owned by a bucket holding
           nobody is not a statement about the frontier the other row is a share of. */
        if (out->br_born_min == 0 || br->sub_born < out->br_born_min) out->br_born_min = br->sub_born;
        if (br->sub_born > out->br_born_max) {
            out->br_born_max = br->sub_born;
            /* …AND THE BUCKET ITSELF, RETAINED RATHER THAN COPIED, for the reason the crowd's pointer is:
               writing the three minter rows HERE would put the maximum and its companions in one statement,
               and `br_minter_live + br_minter_gone == br_born_max` would be an assignment compared with
               itself. The pointer is dereferenced once, after the walk, so the left side of that identity is
               a second reading of the bucket's own pair and the right side is this running maximum.
               THE MINT COUNT IS THE SELECTOR AND THE LIVE COUNT IS NOT, which is the whole difference from
               the pointer moved a few lines up: this names the arm that has TAKEN the most arms and that one
               names the arm that HOLDS the most, and they are one bucket only while nothing has been shed.
               STRICT `>` FOR THE SAME REASON THE CROWD'S IS: two buckets at one mint count leave the maximum
               and the pointer both at the first, so they cannot name two different buckets. */
            g_wfq_census_minter = br;
        }
    } else {
        /* THE EMPTY HALF, AND IT IS NOT A CORNER — branch_take's own note above says a bucket holding nobody
           is the ORDINARY state of any frontier whose boot flow has finished, because the family-root door
           takes exactly such a bucket on purpose to keep boot's burn inside the identity. That burn is real
           receipt and it is what makes `br_us_sum` the wrong denominator for a live arm's share. */
        /* …AND WHICH BUCKETS MAY BE HERE AT ALL, ASSERTED RATHER THAN ARGUED, BECAUSE IT IS WHAT MAKES
           `branches` MINUS THE LIVE-BEARING COUNT A KNOWN QUANTITY INSTEAD OF A RESIDUE. The member door
           passes `f->acct->branch` for a LIVE `f`, and that member's own mint has not been shed, so a bucket
           reached that way holds at least one and cannot arrive here. Everything here therefore came through
           the family-root door, where `Flow.family` names a ROOT (`up == NULL`, asserted at the dispatch) and
           a root's bucket stands at the `sub_born == 1` flow_new wrote — so `live == 0` is its own flow having
           departed, and acct_depart clears `owner` BEFORE it raises `sub_gone`, which is why both halves are
           readable at one instant.
           IT IS NOT THE PARTITION IDENTITY RESTATED, WHICH IS THE TEST THIS FILE APPLIES TO EVERY ADDED
           ASSERTION. `br_live_sum == members` compares two SUMS and is satisfied by any COMPENSATING pair —
           one bucket short a mint and another over by one sum correctly while a live member stands in a bucket
           whose count says nobody does. That pair passes the partition, leaves `br_live_min` untouched (the
           extrema are folded in the arm above and an empty bucket never reaches them), and fires here. */
        DCHECK(br->up == NULL && br->owner == NULL,
               "the census took a branch bucket with no live member that is not a departed family root — the "
               "member door only ever reaches a bucket it is itself counted in, so a NON-ROOT bucket here is a "
               "join that raised `sub_born` for one bucket while leaving `branch` naming another, or a "
               "departure that raised `sub_gone` twice, and a root still OWNING its flow here is a root that "
               "has been forked into. Either way a live member is standing in a bucket whose own count says "
               "nobody is, so `branches` minus the live-bearing buckets is no longer the departed roots and "
               "every per-branch extremum is about to be published over a population nobody can name");
        out->br_empty_us += br->sub_us;
    }
    if (out->branches == 1 || br->sub_us > out->br_us_max) out->br_us_max = br->sub_us;
    if (out->branches == 1 || br->sub_us < out->br_us_min) out->br_us_min = br->sub_us;
}

/* What the ordering is made of; flow.h says what each row answers and why `val` alone cannot. A separate scan, not
 * a fifth question for flow_pick: flow_pick's four questions are all the order and share one comparator, while
 * this decides nothing and reads the order's inputs, so sharing that scan would give the pick accumulators it does
 * not need. It reports the terms and their sum (flow.h says why). `val_top` is flow_best's own reward, so the top
 * of the census and the top of the order are the same flow by construction. */
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
    /* AND THE MEMBER ITSELF, because `deliv_w_gap`'s reading needs a TERM and a weight is a sum — see flow.h's
       `deliv_w_gap_vis`. Held as a pointer rather than as a copied count so the row cannot come to be about a
       different member than the maximum above it: one `if` writes both. */
    const Flow *deliv_holder = NULL;
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
    /* …AND THE DEEPEST ROW OF THE DOCUMENT'S PROGRAM TABLE ANY MEMBER IS STANDING AT, WITH THE BEST WEIGHT
       OFFERED THERE AND HOW MANY STAND WITH IT — held here for the three maxima above's reason exactly:
       `cur_deep_w_gap` is a DIFFERENCE against a top this scan does not know until flow_best runs below, so
       the population is COLLECTED here and only ATTRIBUTED to the front there. The `have_` flag is what keeps
       "no member standing" from being spelled the same way as "a member standing at row 0", which matters
       here more than at the gaps above: row 0 is where every member of a fresh frontier is. */
    int cur_deep = 0;
    long cur_deep_live = 0;
    double cur_deep_w = 0.0;
    int have_cur_deep = 0;

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
    /* …AND THE SHARE OF IT THAT LEFT, ASSIGNED IN THE SAME BREATH FOR THE SAME REASON AND FOR ONE MORE: it is
       the half of the distribution the two gauges above structurally cannot see, so publishing it beside them is
       what makes their partition checkable from OUTSIDE this process, where the DCHECK below is compiled out. */
    out->picks_departed = g_picks_departed;
    /* …AND ITS UNFRAMED SUBSET, ASSIGNED IN THE SAME BREATH AND FOR THE SAME REASON: it is a count of
       dispatches this instance has made, not a reading of this walk, so it is the same number on an empty scan
       as on a full one. Publishing the two together is what makes the containment below checkable from
       outside this process on the document, where the DCHECK is compiled out. */
    out->unframed_picks_lifetime = g_unframed_picks_total;
    /* …AND ITS READY SUBSET, ASSIGNED IN THE SAME BREATH FOR THE SAME REASON — a count of dispatches this
       instance has made, not a reading of this walk, so publishing the three together is what makes both
       containments checkable on the document where the DCHECKs are compiled out. */
    out->ready_picks_lifetime = g_ready_picks_total;
    out->svc_max = out->svc_min = out->svc_fam_max = out->svc_fam_min = 0;
    /* …AND THE TWO ROWS THAT SAY WHAT THE NOTCH ABOVE COSTS TO ASK, cleared beside it because they are read
       over the same walk and off the same quantity (flow.c's flow_silence_phase). The residue map is cleared
       HERE and not at its declaration, because it is a reading of THIS frontier and a byte surviving from the
       previous sample would count a phase no member is standing at. */
    out->sil_phases = out->sil_carry = 0;
    memset(g_phase_seen, 0, sizeof g_phase_seen);
    out->families = 0;
    out->epoch_away_live = out->epoch_away_walk = 0;
    out->branches = 0;
    out->br_live_max = out->br_live_min = out->br_live_sum = 0;
    out->br_born_max = out->br_born_min = 0;
    out->br_us_max = out->br_us_min = out->br_us_sum = 0;
    out->br_crowd_live = out->br_crowd_born = 0;
    out->br_crowd_us = 0;
    out->br_minter_live = out->br_minter_gone = 0;
    out->br_minter_us = 0;
    out->br_held_us = out->br_empty_us = 0;
    /* AND THE RETAINED BUCKET, CLEARED BEFORE THE WALK AS WELL AS AFTER IT IS READ. A pointer surviving from
       the previous census names a node this one may not reach and a later departure may already have freed,
       so the crowd rows would be a reading of a bucket that is not this frontier's — or of memory that is no
       longer a bucket at all. */
    g_wfq_census_crowd = NULL;
    g_wfq_census_minter = NULL;
    out->br_depth_max = 0;
    out->br_fan_max = out->br_fan_sum = 0;
    out->br_fan_depth = 0;
    /* THE TWO INSTANCE TOTALS, WHICH ARE NOT READINGS OF THIS WALK — assigned unconditionally, exactly as
       `picks_lifetime` and `nonreward_max` are, so they are the same numbers on an empty scan as on a full
       one. They are the other two terms of the burn identity asserted at the end of this function, and
       publishing all three is what makes that identity checkable from outside the process. */
    out->br_retired_us = g_branch_retired_us;
    out->charged_us = g_charged_us;
    /* A FRESH MARK FOR THE BUCKET WALK, sharing this scan's generation VALUE and using a mark of its own,
       because a ROOT is both a family and a branch and one field cannot record being counted once as each. */
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
    /* …AND THE TWO HALVES OF THE READY ROW, ZEROED ON THEIR OWN LINE RATHER THAN CHAINED ONTO THAT ONE so the
       partition they state is visible as one statement. A half left un-zeroed would accumulate across censuses
       while the total it is a half of did not, which is exactly what the sum assert at the end of this scan
       fires on — the reset and the identity are one contract. */
    out->jobs_ready_task = out->jobs_ready_micro = 0;
    out->mem_unframed = 0;
    out->job_w_gap = 0.0;
    out->deliv_ready = out->deliv_framed = out->deliv_owed = 0;
    out->deliv_w_gap = 0.0;
    /* …AND THE TWO ENDS OF THAT GAP, ZEROED BESIDE IT AND NOT SEPARATELY, because they are only ever WRITTEN
       beside it: a frontier with no ready holder states no gap and states no counts, and a reader who found
       one of the three set and the others not would be holding a gap about members this scan never found. */
    out->deliv_w_gap_vis = out->w_top_vis = 0;
    /* …AND THE SAME SENTENCE FOR THE DOCUMENT-PROGRESS GAP AND ITS POPULATION, zeroed together for the reason
       the delivery triple is: they are written in ONE branch below, so a reader who found the cursor set and
       the gap not would be holding a distance to members this scan never found. */
    out->cur_deep = 0; out->cur_deep_live = 0; out->cur_deep_w_gap = 0.0;
    for (i = 0; i < g_flows_n; i++) {
        const Flow *f = g_flows[i];
        int64_t s = flow_service_notch(f);
        /* …AND THE OTHER SIDE OF THE AWAY POPULATION'S IDENTITY, COMPUTED FROM THE ACCESSOR WHILE THE
           MAINTAINED COUNT IS FOLDED A FEW LINES DOWN FROM THE FAMILY DOOR. This is the direct reading —
           every member whose flow_own_silence is non-zero stands away from its family's epoch base — and it
           is deliberately NOT the same route as `epoch_away_live`, which is four incremental statements at
           four sites. Two maintainers, one walk, one instant: that is what makes the assertion at the end of
           this function a check rather than a sum compared with its own summands, and what makes a fifth
           transition site nobody found FIRE instead of drifting. */
        if (flow_own_silence(f) > 0) out->epoch_away_walk++;
        /* THE SUM THE PICK USES, not only the terms it is made of — see flow.h for the reading that went wrong
           without it. One call per member, and this scan still decides nothing. */
        double w = flow_weight(f);
        /* …AND PRICED WHERE IT IS TAKEN, exactly as flow_pick prices its own — weight EVALUATIONS and not loop
           trips, so the two rows are the same quantity and a reader may divide one by the other. This walk
           skips nothing, so its count is the frontier's size per sample by construction, which is what makes
           the identity below an assertion rather than a restatement. */
        g_scan_weights[FLOW_SCAN_CENSUS]++;
        /* …AND WHERE THIS MEMBER STANDS IN THE DOCUMENT'S OWN PROGRAM TABLE, KEPT FOR THE DEEPEST ROW ANYBODY
           IS AT — see flow.h. Rewritten WHOLESALE when a deeper member is met and accumulated when an equal
           one is, because the population is "the members at the maximum" and the maximum is not known until
           the walk ends; a running maximum that only ever climbed would leave the count summing every row it
           passed through. It costs no weighing: `w` one line up is this member's, taken for `w_min` anyway.
           `Flow.script_i` AND NOT A DERIVED DEPTH, deliberately: that is the same field cold.c's
           `program_cursors` histogram buckets on, so the two rows are in ONE unit and a reader may check this
           maximum against that histogram's top non-empty index. A second spelling of "how far has this member
           got" would be two quantities one reader would subtract. */
        if (!have_cur_deep || f->script_i > cur_deep) {
            have_cur_deep = 1; cur_deep = f->script_i; cur_deep_live = 1; cur_deep_w = w;
        } else if (f->script_i == cur_deep) {
            cur_deep_live++;
            if (w > cur_deep_w) cur_deep_w = w;
        }
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
        /* AND HOW MANY HOLD NO FRAME, taken HERE rather than off the cold line so that it and `members` are
           one sample from one walk — it is what separates `jobs_ready`'s two zeroes, and a count from another
           instant cannot separate anything.
           IT IS NOT THE JOB ARM'S PREDICATE AND HAS NOT BEEN SINCE THAT ARM WAS RE-SPELLED, WHICH IS WHY THIS
           ROW'S OWN CLAIM IS REWRITTEN RATHER THAN DELETED. It read "spelled with the same predicate the job
           arm below is written against, which is the whole content of the assert at the end of this scan",
           and that was true only while the job split asked `frame` alone. The split asks flow_stack_empty now,
           because HTML §8.1.4.4 "Calling scripts"' clean up after running script step 3 has a SECOND half — a
           row at the cursor marked DYN_POS_IMMEDIATE — and this row still counts `frame == NULL`. The two are
           a CONTAINMENT and never an identity, which the scan's tail already says in its own words; a reader
           who re-derives the retired sentence re-spells one of them to match the other, and `memUnframed` then
           stops being the upper bound the ready rows are published against.
           SO THE CONTAINMENT IS ASSERTED PER MEMBER, WHICH IS THE ONE THING THE TAIL ASSERTS CANNOT DO. Those
           two are written over TOTALS (`jobs_ready == 0 || mem_unframed > 0`) and their messages rest on the
           words "by construction" — a claim that flow_stack_empty's FIRST LINE and THIS line agree, held by
           convention across five thousand lines of one file. Their own paragraph states what they gave up:
           "what the assert stopped being able to catch is a re-spelling of `mem_unframed`". This catches it,
           because the assert and the count read ONE expression: re-spell the count and the assert follows the
           re-spelling, so a predicate that stops containing flow_stack_empty's population fires at the member
           that breaks it rather than reaching the published rows as a bound that is not one.
           ONE-SIDED, AND THE CONVERSE IS FALSE BY DESIGN RATHER THAN MERELY UNASSERTED — which is the clause a
           reader needs before reaching for `==`. A member holding no frame whose cursor names a
           DYN_POS_IMMEDIATE row is HTML §4.12.1.1 "Processing model"'s "Otherwise, immediately execute the
           script element el, even if other scripts are already executing": it is inside `mem_unframed` and
           flow_stack_empty refuses it, so an equality here would fire on every document that injects a
           script from a script.
           IT CANNOT FAIL AT THIS REVISION AND THAT IS THE POINT — it is a design invariant and not a test, in
           §Offensive-programming's sense. What makes it a check rather than a tautology is that its two
           operands live in two functions five thousand lines apart and are independently editable, which is
           exactly the drift the tail's paragraph names and declines to cover.
           HOW ITS ABSENCE WOULD SHOW: `jobsReady` or `delivReady` above zero beside a `memUnframed` that no
           ready member is inside — a state both tail asserts PASS, because a total above zero satisfies them
           whatever population it was taken over. */
        {
            const int unframed = (f->frame == NULL);
            DCHECK(!flow_stack_empty(f) || unframed,
                   "a member whose JavaScript execution context stack is EMPTY was not counted as holding no "
                   "frame — flow_stack_empty's first line is `if (f->frame) return 0;`, so every member it "
                   "admits is inside `mem_unframed`, and the two ready-row asserts at the end of this scan "
                   "state that containment as holding BY CONSTRUCTION. It does not hold: this row's predicate "
                   "and that function's first conjunct are two spellings and one of them has moved. "
                   "`memUnframed` is published as the upper bound `jobsReady` and `delivReady` are read "
                   "against, so from here it is a bound over a population neither ready arm is inside");
            if (unframed) out->mem_unframed++;
        }
        /* THE JOB BACKLOG SPLIT BY WHAT IT IS WAITING ON — the three states flow.h names, decided by the two
           predicates the engine already asks and in the order it asks them. flow_pick refuses a host-owed
           member outright, so that question comes first; among the members the pick will consider, HTML
           §8.1.4.4 "Calling scripts"'s clean up after running script step 3 ("If the JavaScript execution
           context stack is now empty, perform a microtask checkpoint") is what every job arm of flow_step is
           under. Whatever is left waits on RANK, and it is the only one of the three the ordering can move.
           Written as if/else-if/else rather than three tests so the classes are disjoint and exhaustive BY
           CONSTRUCTION: a member holding jobs has exactly one reason, and a fourth reason has nowhere to be
           folded into.
           AND `frame` IS NOT THAT STACK, WHICH IS THE RETIRED ARGUMENT AND IS REWRITTEN HERE RATHER THAN
           DELETED BECAUSE IT IS THE ONE A READER RE-DERIVES. This arm read `f->frame` under a note saying so,
           and the delivery split twenty lines down said the job rows read `frame` "because that is what THEIR
           arms read". flow_run_one_job has exactly TWO call sites and NEITHER of them reads `frame` alone: the
           checkpoint arm is guarded on flow_checkpoint_due, which is `flow_job_microtask && flow_stack_empty`,
           and flow_stack_empty is this engine's own statement of HTML §8.1.4.4's sentence — a live frame OR
           a row at the cursor marked DYN_POS_IMMEDIATE, that row being the synchronous tail of the program
           that queued it, so the stack is not empty for it either. The quotation was right and the predicate
           named for it was one conjunct short, which is the shape a reviewer passes: the spec sentence checks
           out.
           WHAT IT COST IS A ROW POINTING AT THE WRONG COMPONENT. `jobs_ready` is published as "waits on RANK
           ALONE … the only one of the three the ordering can move", so a member the LADDER holds off was
           reported as one the ORDER had not got to, and `job_w_gap` — taken over the same holders — then read
           ~0 and said the backlog "is not an ordering problem at all". Those two sentences together close the
           question, and they closed it toward the scheduler.
           ASKED THROUGH THE ARM'S OWN GUARD AND NOT A SECOND SPELLING OF IT, which is also what keeps this
           honest as the arm moves: flow_stack_empty is the function flow_checkpoint_due calls, so an edit to
           the checkpoint's precondition reaches this census without anybody remembering to come here.
           NAMED RESIDUAL — this row is CORRECT for a MICROTASK and NARROWER than the question for a TASK.
           NOT COVERED: a member that satisfies flow_stack_empty and holds a job with JOB_TASK set while its
           cursor names a row the flow CAN RUN. flow_step's task arm used to bind its `else` to
           `f->script_i < f->dyn_n`, which made the uncovered population every member whose cursor named a row
           AT ALL; it binds to `seq_compiles` now — `a program STARTS on this step` — so a member parked on an
           external script row does reach its jobs and this row is exact for it. What is left is the member
           whose next row is RUNNABLE: the sequence arm takes every such step, so no pick reaches that job and
           this row still calls it ready. THAT HALF IS EXACT AND HAS BEEN MEASURED EXACT: on two independent
           real-page runs every pick that ever reached an unframed member took the sequence arm and raised
           `progStarts`, and `_jobsRun` stayed at zero — `progStarts` equals the number of frame-ends the run
           consumed plus one (the boot flow, which had no prior frame-end) in BOTH runs, 22 against 21 and 17
           against 16. Not one pick of this population has ever reached the job arm.
           AND THAT POPULATION NOW HAS A COUNT RATHER THAN A DERIVATION: `jobs_ready_task`, raised in the arm
           just below this one. This residual's NOT COVERED clause is a claim about the TASKS inside
           `jobs_ready`, and until that row existed the only way to size it was the four-bucket step partition
           plus the argument above. IT IS NOT RETIRED BY THAT ROW — the row measures the population, it does
           not stop the population being reported as rank-ready — and what it buys is that the two halves of
           this row's error are separable: the microtask half is exact, and the task half is exactly
           `jobs_ready_task`.
           NEXT DIFF: REWRITTEN TWICE NOW, AND THE SECOND REWRITE IS THE ONE WORTH READING, BECAUSE THE CLAUSE
           THAT REPLACED THE FIRST WAS ITSELF REFUTED — BY THE ROW THE SAME PARAGRAPH ASKED FOR.
           THE FIRST CLAUSE read: the per-source task queue that arm's own residual names — a source on a
           `jobs` entry as it is now on a row — after which the arm's reachability stops depending on the
           cursor at all and the ready/framed line is exact for both kinds.
           THE SECOND CLAUSE said that one moves no number: `it decides WHICH ARM RUNS FOR A MEMBER THAT IS
           PICKED, and this population is not being picked at all`, measured on two runs by the four-bucket
           step partition — `units + midProgram + parked + checkpointOwed == steps` is asserted at
           engine_frontier_census, and every bucket an unframed member's step could land in was held flat over
           the interval (`progStarts` frozen, `jobsRun` 0, `run-a-task` 0, `microtask-checkpoint` 0,
           `unitParked` 0, `unitCheckpointOwed` 0, `deliver-one-reply` flat at the reply count) — so the
           population received ZERO of 21983 and 3554 dispatches while growing to 273 and 124 members.
           THE PREMISE OF THE SECOND IS FALSE AND `ready_picks_lifetime` IS WHAT FALSIFIED IT, which is the
           enumeration four paragraphs down working exactly as written: it names three readings, says `above
           zero with _jobsRun flat` is the second of them — `the dispatch DOES take it and flow_step's LADDER
           declines the job at an arm above the one that would run it` — and a real-application drive answered
           `readyPicksLifetime 243` against `_jobsRun 0`, `jobsReadyTask 104` and `run-a-task 0`. The
           population IS being picked. So the clause was not stale: it was an inference from a SUPERSET row
           (`unframed_picks_lifetime`) standing in for a subset the tree could not then measure, and the very
           row this paragraph asked for is the one that took it apart.
           WHAT THAT COST AND WHAT IT DID NOT. Nothing was built to it, because the row landed first — which is
           the mechanism working rather than luck, and is the whole argument for publishing a row instead of
           an inference. What it WOULD have cost is exact and is worth stating, because the clause reads as a
           refusal: a lane holding it declines the ladder fix and goes to flow_pick, which the same census
           exonerates (`jobWGap` 0 at every census that has one — the order already ranks this population at
           its front).
           THE METHOD IS THE FINDING AND NOT THE SENTENCE. Both clauses were written from a step partition
           whose buckets are counted over WHOLE RUNS and read as though they were counted over the population
           in question; a bucket held flat says the population ran nothing, and `nothing was picked` and `what
           was picked declined at an arm` produce the identical flat bucket. A reading that cannot separate
           two states may not be spent naming one of them, which is this file's own three-states rule met on
           the inside. The row that CAN separate them was already named here as the next diff; the clause
           should have said `unknown until that row exists` and said `moves no number` instead.
           THE LADDER FIX HAS NOW LANDED (engine.c's task ladder binds to ARRIVAL rather than to the carrier),
           and it is NOT the first clause either: that one said one queue per source, and what the ladder
           needed was one CLOCK across the carriers — strictly wider, since it orders a source no producer has
           written yet. The `jobs` entry does carry a stamp now (flow.h's job-record bullet), so the half of
           the first clause that was about plumbing is built and the half that was about per-source queues is
           superseded rather than done.
           WHAT IS UPSTREAM OF THE ARM ORDER IS THEREFORE THE QUESTION, AND THIS ROW'S OWN PAIR STATES IT AS AN
           IMPOSSIBILITY RATHER THAN A MECHANISM. `job_w_gap` is `w_top` minus the best weight any member of
           this population offers, both taken through `flow_weight` and `w_top` taken from flow_best's own
           return — one function, one walk — so a gap of zero says the front of the order IS one of these
           members. It reads zero at EVERY census of both runs that has one, and `cur_deep_w_gap` reads zero
           beside it. THREE readings survive and they take opposite work: the order ranks this population at
           its front and the dispatch does not take it; the dispatch DOES take it and flow_step's LADDER
           declines the job at an arm above the one that would run it; or `w_top` and `job_w_max` are not the
           quantities the dispatch compares at all. The third is written into the enumeration rather than
           corrected beneath it, which is what this paragraph's own retirement asked for: a two-way choice
           between a SCHEDULER fault and a COMPARATOR fault had no room for a LADDER fault, and the NOT COVERED
           clause naming the ladder was six lines up while the pair below it went on sending the reader to
           flow_pick.
           THE FIRST TWO ARE A ROW NOW AND NOT AN INFERENCE — `ready_picks_lifetime`, published beside
           `unframed_picks_lifetime`, whose legend in flow.h states the triple: zero here with the superset
           non-zero is the first, above zero with `_jobsRun` flat is the second. `unframed_picks_lifetime`
           alone could only refute `never taken` for the SUPERSET, which is why naming one arm on the strength
           of it would have been the wrong-narrowing move this file refuses everywhere else. What IS
           established is that the reading `jobs_ready` publishes — `waits on RANK ALONE` — is not an ordering
           problem the weight can be shown to have, because the same census says these members are already at
           the front of it.
           HOW ITS ABSENCE SHOWS: a census publishing `jobsReady` above zero with `jobWGap` at zero, on a run
           whose LIFETIME `_jobsRun` never leaves zero — a backlog standing at the front of the order that the
           order cannot move, which is the pair this row exists to make impossible to say. IT HAS FIRED, on
           every real-page census of both runs, which is why the paragraphs above exist: the clause was written
           as a thing to watch for and it is a description of the ordinary state of this engine on a real
           document. THE RETIREMENT CLAUSE THAT STOOD HERE IS SATISFIED AND THE ROW IT ASKED FOR IS
           `unframedPicksLifetime`, raised in flow_credit_pick and published beside `memUnframed`. What that
           retires is this paragraph's standing in for a MEASUREMENT: the two readings are separated by a row
           now, and the legend that says which value means which lives at `unframed_picks_lifetime` in
           flow.h, beside the gauge it is the counter for, where a reader who meets the number will be. THE ROW HAS NOW
           BEEN READ AND THE READING IS AT THAT LEGEND, so what is left standing here is the analysis and not
           the wait: a reader holding the digit needs the statement of what each outcome implies, and the
           NOT COVERED clause above is the arm the answer turned out to be about. RETIREMENT: this paragraph
           goes when the ladder's own arm order is settled, because the reachability question it exists to
           frame is then answered at flow_step rather than inferred from a census here. */
        {
            int jn = flow_job_pending(f);
            if (jn > 0) {
                if (flow_host_owed(f))         out->jobs_owed   += jn;
                else if (!flow_stack_empty(f)) out->jobs_framed += jn;
                else {
                    int jt, jm;

                    out->jobs_ready += jn;
                    /* …AND SPLIT AGAIN ON THE ONE AXIS THAT DECIDES WHICH ARM OF flow_step CAN TAKE THE JOB —
                       see flow.h. The split above says what a job WAITS ON; this says which arm DISPATCHES it,
                       and a MICROTASK's arm (the checkpoint) stands ABOVE the program sequence while a TASK's
                       stands below it, so `jobs_ready` calls rank-ready two populations of which one is also
                       waiting on the ladder to reach its arm.
                       TAKEN HERE, INSIDE THE ARM THAT ALREADY ADMITTED THIS MEMBER, so the queue walk it costs
                       is paid only for the population `mem_unframed` bounds and never once per member — an
                       instrument that ran at the frequency of the frontier would be measuring a different run.
                       BOTH HALVES COME FROM flow_job_kinds AND NEITHER IS `jn` MINUS THE OTHER, because the
                       sum asserted at the end of this scan is what states the partition, and a subtracted half
                       makes that assert one whose two sides cannot disagree. */
                    flow_job_kinds(f, &jt, &jm);
                    out->jobs_ready_task  += jt;
                    out->jobs_ready_micro += jm;
                    /* THE BEST OF THEM, against which `job_w_gap` is taken below. A maximum and not a first
                       hit: the question is how far the backlog's BEST claim stands from the front of the
                       queue, and any other holder is further still. */
                    if (!have_job_holder || w > job_w_max) { job_w_max = w; have_job_holder = 1; }
                }
            }
        }
        /* AND THE SAME THREE QUESTIONS OF THE REPLY BACKLOG — see flow.h. Asked through the delivery arm's
           OWN guard (flow_stack_empty) rather than through `frame` alone, because that is the predicate
           engine.c's arm is written against and this row exists to say whether that arm can run. The job rows
           above are now asked the same way and for the same reason; the clause that stood here saying they
           read `frame` "because that is what THEIR arms read" was a claim about flow_step that flow_step does
           not support, and its repair is written out at the job split itself. Same if/else-if/else shape, so
           the three classes are disjoint and exhaustive by construction and a fourth reason has nowhere to
           hide.
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
                if (!have_deliv_holder || w > deliv_w_max) {
                    deliv_w_max = w; have_deliv_holder = 1; deliv_holder = f;
                }
            }
        }
        if (s > out->svc_max) out->svc_max = s;
        /* …AND THE SUB-QUANTUM REMAINDER `s` THREW AWAY, WHICH IS WHAT DECIDES WHETHER ASKING THE ORDER HAS
           TO WALK AT ALL. `s` is `own / S` and this is `own % S`: the member's PHASE against the family's,
           and the only per-member quantity in flow_weight that moves between two frontier generations
           (flow_silence_phase states the decomposition). Counted here because the walk is already holding the
           member and has already paid for the read `s` came from.
           DISTINCT VALUES AND NOT A SPREAD, because the two answer different questions and only one of them
           is about an index. A spread says how far apart two members stand; what an index over this key needs
           to know is HOW MANY GROUPS the frontier partitions into, and `sil_phases: 1` is the strongest
           answer the ARITHMETIC permits — every member crosses the carry boundary at the same instant, so the
           bit is a COMMON OFFSET, nothing reorders between generation bumps at all, and a single cached
           maximum is exact. Any larger reading is the number of ranges a sweep has to move through.
           THIS USED TO READ `the strongest possible answer` FLAT, AND THE ARCHIVE CONTRADICTS THAT AS A CLAIM
           ABOUT THIS ENGINE — every `sil_phases == 1` in this repository's logs stands at `members == 1`, and
           a live page reads about half its frontier. solver/flow.h carries the measurement, the corpus it was
           taken over and the retirement condition; it is repaired HERE TOO because one sentence held at three
           sites is three chances to be stale, and repairing the one a reader happens to find is how the other
           two get certified. */
        {
            int64_t ph = flow_silence_phase(f);
            if (!g_phase_seen[(size_t)ph]) { g_phase_seen[(size_t)ph] = 1; out->sil_phases++; }
            out->sil_carry += flow_silence_carry(f);
        }
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
                /* …AND THIS FAMILY'S MAINTAINED AWAY COUNT, FOLDED AT THE ONE DOOR THAT VISITS EACH FAMILY
                   EXACTLY ONCE. `census_gen` is what makes this a SUM over distinct families rather than a
                   weighted count, and it is the mark this door already sets — so the fold costs nothing and
                   cannot double-count a family however many of its members the walk reaches. */
                out->epoch_away_live += f->family->away_n;
                /* …AND THE ROOT'S OWN BRANCH BUCKET, OPENED HERE BECAUSE THIS IS WHERE THE ROOT IS ALREADY IN
                   HAND AND ALREADY GUARDED. A root's bucket holds exactly ONE member — the root flow itself,
                   since every arm it forks opens a bucket of its own — so the instant the root flow departs,
                   its node is still held alive by its children's `up` references while NOTHING names it
                   through `branch` any more, and the thread time the root burned (boot's, on a real page)
                   would be in neither the live sum nor the retired term for the rest of the session. That is
                   the one way the burn identity below could fail on a healthy frontier, and it is closed by
                   the door rather than by a term in the identity. Once per distinct family per census, which
                   is the frequency a root bucket wants; the node's own mark makes it idempotent against the
                   member door below, which reaches the same node while the root flow is still standing. */
                DCHECK(f->family->branch == f->family,
                       "a family ROOT is not a branch bucket of its own — flow_new points a fresh node's "
                       "`branch` at itself and only a fork ever moves it, so this root has been through an "
                       "inheritance it cannot have had, and the thread time its own flow burned is about to "
                       "be attributed to somebody else's subtree");
                branch_take(out, f->family);
            }
            if (fam > out->svc_fam_max) out->svc_fam_max = fam;
            /* …AND ITS FLOOR, WHICH IS WHAT SAYS THE TERM IS ORDERING RATHER THAN OFFSETTING — see flow.h.
               A maximum states how LARGE the family half is; only the pair states how much of it any
               comparison in this frontier can see, and on a single-family frontier that is exactly zero. */
            if (i == 0 || fam < out->svc_fam_min) out->svc_fam_min = fam;
        }
        /* AND THE BRANCH BUCKET THIS MEMBER'S SUBTREE IS COUNTED IN — the accounting scope BETWEEN the two the
           rows above read, and the one this census could not previously reach at all. `svc_max`/`svc_min` are
           the MEMBER's silence and `svc_fam_max`/`svc_fam_min` are the FAMILY ROOT's, and on a page whose
           flows all descend from boot the second pair is a common offset that orders nothing — so neither of
           them can say how much of the thread each SIDE of a branch received, which is the one question a
           frontier that grows without draining poses.
           COUNTED BY IDENTITY AND NOT BY VALUE, in the same walk and by the same mark trick the family count
           beside it uses: two buckets standing at one burn are two buckets. Each distinct bucket is opened
           exactly once per census however many arms it has standing, which is what makes the sum below a
           partition of the frontier rather than a weighted count of it.
           THE READING, STATED HERE SO IT IS NOT RE-DERIVED AT EVERY SITE. `br_live_max` against `members` is
           how concentrated the frontier is in one side of one top-level branch; `br_us_max` against
           `charged_us` is how concentrated the THREAD is; `br_born_max` against `br_live_max` separates a
           bucket that MINTS unboundedly from one that merely HOLDS a lot right now, and those two take
           opposite diffs.
           AND THIS SAID `THE OTHER SIDE OF THAT BRANCH IS THE REMAINDER OF EACH', WHICH IS THREE MAXIMA OVER
           THREE POPULATIONS READ AS ONE ARM'S THREE PROPERTIES. Each is a fact about whichever bucket happens
           to own it, and on a frontier with more than two buckets they need not be one bucket at all — the
           burn maximum in particular is routinely a DEPARTED family root holding no live member, which the
           door above takes deliberately, so a reader dividing that receipt by the live maximum's membership
           was reporting boot's thread time as the crowd's. The crowd rows read after this walk are what make
           the sentence true: they state ONE bucket's membership, mint and receipt, and each published total
           is what the remainder is taken from. */
        {
            FlowAcct *br;
            DCHECK(f->acct != NULL,
                   "a live member of the frontier has no fork-tree node — the node is minted at flow_new and "
                   "released only at the departure that also removes the flow, so this member is standing in "
                   "the registry after its account was torn down and every burn charged through it is lost");
            br = f->acct->branch;
            DCHECK(br != NULL,
                   "a live member's fork-tree node names no branch bucket — flow_new points every node at "
                   "one and flow_fork_inherit only ever re-points it at another, so this member's thread "
                   "time is being charged to no side of any branch");
            /* THROUGH TWO DOORS, AND THE SECOND ONE IS THE BURN IDENTITY'S WHOLE SOUNDNESS. Every live
               bucket must be reached by this walk or its lifetime burn is in neither the live sum nor the
               retired term, and the identity below then fails on a healthy frontier. A NON-ROOT bucket is
               reached by its own subtree: a node is alive only while it has a live-owner descendant, and
               every descendant of a non-root bucket names that bucket. A FAMILY ROOT is the exception, and
               it is a structural one rather than a corner: the root's bucket holds exactly ONE member — the
               root flow itself, since every arm it forks opens a bucket of its own — so the moment the root
               flow departs, its node is still held alive by its children's `up` references while NOTHING
               names it through `branch` any more. Its own thread time (boot's, on a real page) would then be
               charged to no published bucket for the rest of the session.
               `Flow.family` IS that door — it points straight at the root, a root's `branch` is itself, and
               any live member of the family reaches it — and the door is opened in the FAMILY-ROOT block
               above rather than here, deliberately. That block already tests `f->family` before dereferencing
               it and already fires exactly once per distinct family per census, which is exactly the
               frequency a root bucket wants; opening it HERE would have been this file's FIRST release-mode
               dereference of a pointer that only a DCHECK guards, which is a promotion an observation has no
               business making. Marking through the node's OWN generation is what stops the root being counted
               twice while its own flow is still standing — one bucket reached two ways, not two buckets. */
            branch_take(out, br);
            /* …AND HOW DEEP IN THE FORK TREE THE DEEPEST LIVE MEMBER SITS, which is not a fact about buckets
               and is taken here because this is where the node is already in hand. It is what says whether the
               tree this engine builds is a STAR or a CHAIN.
               IT WAS ADDED AS A PRECONDITION AND THE PRECONDITION IS DISCHARGED, which is a different standing
               from the one it had. The residual at FlowAcct's `up` asked it whether a per-node aggregate could
               be maintained by walking to the root; it answered STAR — 5..6 against a frontier of 75113 — so
               the walk is affordable, and the clause was refuted on a cost it had not named (RETENTION, which
               depth cannot see) and on a scope that would have moved a weight term. What the row is FOR now is
               the shape itself, which every other row here is blind to, and the fan below is what answers the
               question the retired clause was reaching for. */
            if (f->acct->depth > out->br_depth_max) out->br_depth_max = f->acct->depth;
            /* …AND WHICH FORK MINTED THIS MEMBER, WHICH IS THE ONE THING THE BUCKET ABOVE SUMS AWAY. A
               bucket is a TOP-LEVEL ARM, so every fork taken deeper lands in its ancestor's bucket and the
               rows above cannot say whether an arm minted the crowd at the first branch or four levels down.
               The fan says exactly that, and it is counted here because the node is already in hand.
               THE KEY IS `up` AND IT IS THE TRUE FORK PARENT, not an approximation of one. `up` has two
               writers: flow_fork_inherit, at the fork, and acct_compress_dead, which runs only from
               acct_depart and only on the DEPARTING flow's own node — so a LIVE member's edge is written once
               and never moved, and the compression only ever shortens the chain above a node that has already
               left. The parent may itself be DEAD and retained by this very member, which is the informative
               case rather than a corner: a walker that forked N arms and then finished is exactly the shape
               this row exists to name.
               NON-ROOT PARENTS ONLY, WHICH IS THE UNCOVERED POPULATION AND NOT A NARROWING FOR CONVENIENCE. A
               fork off a ROOT opens a bucket of its own (flow_fork_inherit's `br` expression), so the rows
               above already separate it; a fork off anything else is summed into its ancestor's bucket and has
               no row anywhere. The gate is spelled with the SAME predicate that decides the bucket, so the two
               cannot drift into disagreeing about which forks the buckets already cover.
               IT IS A GAUGE over the members standing now and may FALL between samples, like `br_live_*` and
               unlike everything on this line carrying `Life`: a fan is how wide a fork stands AT THIS INSTANT,
               and the arms it minted and shed are not in it. `br_born_max` is the lifetime question and it is
               asked at the bucket. */
            {
                FlowAcct *up = f->acct->up;
                /* THE BUCKET A CHILD INHERITS, READ BACK AT THE SITE THAT COUNTS THE CHILDREN — the write
                   side is flow_fork_inherit's `br = parent->acct->up ? parent->acct->branch : sib->acct`, and
                   this asks its NON-ROOT arm at a second instant, long after the fork. Both operands are real
                   program state: a compression that moved a LIVE member's edge, or a `branch` re-pointed by
                   something that is not the fork, separates them. It is what makes the fan identity at the
                   end of this function true by construction rather than by hope — every member the fan counts
                   under a non-root parent is a live member of that parent's bucket, so a fan can never
                   outnumber the bucket holding it.
                   THE ROOT ARM IS NOT ASKED HERE AND THAT IS NOT AN OMISSION: flow_fork_inherit asserts it
                   where it is decided (a root's own `sub_born` staying at 1), and nothing the fan publishes
                   rests on it, because a root-parented member is excluded from the fan by the gate below. */
                DCHECK(up == NULL || up->up == NULL || f->acct->branch == up->branch,
                       "a live member's branch bucket is not the one its fork parent carries — a fork off a "
                       "non-root joins its parent's bucket and nothing else ever re-points `branch`, so this "
                       "member is counted in a subtree it was not forked into and the fan below is about to "
                       "be published as a share of a bucket it is not drawn from");
                if (up && up->up) {
                    if (up->fan_gen != g_wfq_census_gen) { up->fan_gen = g_wfq_census_gen; up->fan_n = 0; }
                    up->fan_n++;
                    out->br_fan_sum++;
                    /* THE DEPTH IS TAKEN WITH THE MAXIMUM AND NOT SEPARATELY, because it is a fact about the
                       node that HOLDS the maximum and a second extremum over depths would be a fact about a
                       different node. `>` and not `>=` keeps the shallowest such fork when two tie, which is
                       the one nearer the bucket the other rows are about. */
                    if (up->fan_n > out->br_fan_max) {
                        out->br_fan_max = up->fan_n;
                        out->br_fan_depth = up->depth;
                    }
                }
            }
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
       and the identity that defines a counter is the one property of it a reader can actually check.
       IT WAS A ONE-SIDED INEQUALITY AND THE ARGUMENT FOR THAT IS RETIRED RATHER THAN DELETED, BECAUSE IT IS
       THE ARGUMENT A READER RE-DERIVES FROM THE TWO ROWS ALONE. It read: "the side it is one-sided on IS the
       statement … the gauge can never exceed the counter, and the DIFFERENCE between them is exactly what the
       retired members took with them". The first clause is true and the second is a claim the inequality does
       NOT make: `live <= lifetime` holds for every distribution of that difference, including one in which no
       departed member ever held a dispatch and the shortfall is a member's `picks` written from somewhere
       else. So the sentence a reader acts on — flow.h's `(picks_lifetime - picks_live) / departures` is what a
       departed member held on average — was stated in the register of something checked and was checked by
       nothing, which is the shape a reader trusts hardest.
       IT IS EXACT NOW BECAUSE THE MISSING ARM EXISTS: every dispatch is credited to exactly one member at
       flow_credit_pick, a member's count starts at 0 at flow_new and is carried by no fork, and flow_remove
       moves it into `g_picks_departed` on the one line that can still read it — so the two halves PARTITION
       the total and the reading above divides a numerator by its own population.
       BOTH ASSERTS STAND AND THEY ARE NOT ONE COPY TWICE: the equality holds with a NEGATIVE departed total
       and the inequality does not, so the pair is what separates a lost credit from a fabricated one, and each
       message names a different next diff. Two comparisons of integers already in hand.
       IT READS THE PUBLISHED FIELD AND NOT THE FILE-STATIC, WHICH IS A DIFFERENCE OF ONE TOKEN AND IS THE WHOLE
       POINT OF THE ROW. The departed total is emitted now (flow.h's `picks_departed`, result_wfq_json's
       `picksDeparted`), so asserting over `g_picks_departed` would check a quantity no reader outside this
       process holds while the one they DO hold went unchecked — two numbers wearing one name, with the identity
       standing over whichever of them the assignment above did not carry. Asserted over `out->`, the dev build
       and the document are checking the same three integers. */
    DCHECKF(out->picks_live + out->picks_departed == out->picks_lifetime,
            "solver/flow.c: the frontier's live members hold %lld dispatches and departed members took %lld, "
            "against %lld the scheduler has ever made — every dispatch is credited to exactly one member at "
            "flow_credit_pick, a member arrives holding none and no fork carries one, and flow_remove moves "
            "the departing member's share into the second of these, so these two are a PARTITION of the third "
            "and a difference is a dispatch credited to a member that left without going through flow_remove "
            "or a `Flow.picks` with a second writer. Every reading of `(picksLifetime - picksLive)` as what "
            "the departed members held is then a quotient over the wrong population",
            (long long)out->picks_live, (long long)out->picks_departed, (long long)out->picks_lifetime);
    DCHECK(out->picks_live <= out->picks_lifetime,
           "the frontier's live members hold MORE dispatches between them than the scheduler has ever made — "
           "flow_credit_pick raises both in one statement and is the only writer of either, so a member's "
           "`picks` has been written from somewhere else and every reading derived from these rows is about "
           "dispatches that did not happen");
    /* AND THE SAME IDENTITY FOR THE SUBSET OF THOSE DISPATCHES THAT REACHED AN UNFRAMED MEMBER, which is the
       one property of it a reader can check and is therefore what makes it a counter rather than a digit.
       CLAUDE.md records the defect this refuses: a count offered as a share of another, raised at a different
       event from its denominator, whose free tell is a subset exceeding the population it claims to be drawn
       from. Here the two are raised in ONE statement block of flow_credit_pick — an `if` beside a `++` — so
       the inequality is by construction and can fail only on an edit that gives one of them a second writer,
       which is precisely the edit that would make every reading of the pair a statement about two different
       dispatch paths. It costs one comparison of two integers already in hand. */
    DCHECK(out->unframed_picks_lifetime <= out->picks_lifetime,
           "the scheduler has dispatched an UNFRAMED member more often than it has dispatched at all: "
           "flow_credit_pick raises both in one statement block, the second conditionally on the first, so "
           "one of them now has a writer elsewhere and the row that separates `memUnframed`'s two readings "
           "is a fraction over the wrong denominator");
    /* AND THE INNER ONE, WHICH IS WHAT MAKES THE NEW ROW A SUBSET RATHER THAN A SECOND OPINION. It is raised
       inside the outer `if` of flow_credit_pick, so this can fail only on an edit that lifts it out — which is
       precisely the edit that would make `readyPicksLifetime` a count over a population `unframedPicksLifetime`
       is not drawn from, and the pair would then be two dispatch paths wearing one ratio. Two integers in hand. */
    DCHECK(out->ready_picks_lifetime <= out->unframed_picks_lifetime,
           "the scheduler has dispatched a READY JOB HOLDER more often than it has dispatched an UNFRAMED "
           "member at all: the ready count is raised inside the unframed count's own `if` in flow_credit_pick, "
           "so it has acquired a second writer and the row that turns `unframedPicksLifetime` from a bound "
           "into a measurement of the job backlog is a fraction over the wrong denominator");
    /* THE FATTEST LIVE BUCKET'S OWN THREE NUMBERS, TAKEN FROM THE ONE NODE THE WALK RETAINED — see flow.h for
       the three states they separate and for why three extrema over a population cannot separate them. This
       is the SECOND reading of that bucket's membership pair: branch_take folded the first into a running
       maximum during the walk, and the identity below compares the two.
       IT RUNS ONCE PER CENSUS AND NOWHERE ELSE. Three loads and a subtraction, off a pointer already in hand
       — no charge-time work, no fork-time work, no walk of any ancestry, and nothing whatever per opcode. The
       only per-bucket cost this instrument adds is the two adds inside branch_take's existing condition.
       A NULL HERE IS `NO LIVE BUCKET WAS SEEN', which is the state the assert further down already says
       cannot arise with members standing; the rows then stay at the zero the reset wrote, and `br_crowd_live`
       is the discriminator a reader needs to tell that zero from a standing crowd that has never been
       charged. */
    if (g_wfq_census_crowd) {
        const FlowAcct *cr = g_wfq_census_crowd;
        out->br_crowd_live = cr->sub_born - cr->sub_gone;
        out->br_crowd_born = cr->sub_born;
        out->br_crowd_us = cr->sub_us;
    }
    g_wfq_census_crowd = NULL;
    /* AND THE BIGGEST-MINTING BUCKET'S OWN THREE, FROM THE OTHER NODE THE SAME WALK RETAINED — see flow.h for
       why the crowd's selector cannot reach this arm and for the three states these separate. This is the
       SECOND reading of that bucket's membership pair, and the identity below compares it against the running
       maximum branch_take folded during the walk.
       IT SHARES EVERY COST SENTENCE THE CROWD'S DEREFERENCE CARRIES: three loads and a subtraction off a
       pointer already in hand, once per census, with no charge-time work and nothing per opcode.
       `sub_gone` IS READ DIRECTLY AND NOT DERIVED, which is what makes the row a second writer's statement
       rather than a rearrangement of the two beside it: the live count is `sub_born - sub_gone` and the shed
       count is `sub_gone`, so their sum is this node's `sub_born` reached by a path the fold never took.
       A NULL HERE IS `NO LIVE BUCKET WAS SEEN'. `sub_born` is at least 1 for every bucket and `br_born_max`
       starts at 0, so the first live bucket the walk reaches sets both the maximum and this pointer — which
       means a NULL is exactly the state the assert further down already says cannot arise with members
       standing, and the rows then stay at the zero the reset wrote. */
    if (g_wfq_census_minter) {
        const FlowAcct *mn = g_wfq_census_minter;
        out->br_minter_live = mn->sub_born - mn->sub_gone;
        out->br_minter_gone = mn->sub_gone;
        out->br_minter_us = mn->sub_us;
    }
    g_wfq_census_minter = NULL;
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
    if (have_deliv_holder && top) {
        out->deliv_w_gap = out->w_top - deliv_w_max;
        /* …AND THE TERM THE GAP IS MADE OF, AT BOTH ENDS, WRITTEN WHERE THE GAP IS — see flow.h. Both are set
           in the SAME branch as the gap and nowhere else, so a reader that finds a non-zero `deliv_w_gap` is
           holding two counts about the two members it is between, and a reader that finds a zero gap is
           holding two zeros for the same reason the gap is zero: there was no ready holder to state one of. */
        out->deliv_w_gap_vis = deliv_holder->visits;
        out->w_top_vis = top->visits;
        DCHECK(out->deliv_w_gap_vis >= 0 && out->w_top_vis >= 0,
               "a WFQ census read a negative visit count at one end of the delivery gap — `visits` is raised "
               "only by flow_credit_visit and reset by nothing, so a negative one is a member whose optimism "
               "term has been computed from a count that cannot exist");
    }

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

    /* …AND HOW FAR THE FRONT STANDS AHEAD OF THE MEMBERS THAT HAVE RUN FURTHEST THROUGH THE DOCUMENT — taken
       here for the identical reason the three gaps above are, and written as ONE branch so the cursor, its
       population and the distance are either all three about members this scan found or all three zero. See
       flow.h for what the reading separates and for why no term of the order may ever read it. */
    if (have_cur_deep && top) {
        out->cur_deep = cur_deep;
        out->cur_deep_live = cur_deep_live;
        out->cur_deep_w_gap = out->w_top - cur_deep_w;
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
    /* …AND THE COUNT AND THE GAP ARE ABOUT ONE POPULATION, WHICH IS THE OTHER HALF OF THIS ROW'S CONTRACT AND
       THE HALF THE SIGN CHECK ABOVE CANNOT SEE. `job_w_gap` is written only under `have_job_holder`, and that
       flag is raised only inside the ready arm, so a census that counts no rank-waiting job states no
       distance — which is why solver/result.c's job-row banner tells a reader to take the two together and
       neither alone. That pairing is true by construction and was recorded only as a sentence.
       WHAT IT CATCHES IS AN EDIT WITH AN ATTRACTIVE REASON: a maximum widened to take a FRAMED or HOST-OWED
       holder so the gap is "more informative" raises the flag outside the arm, after which `jobWGap` is a
       reading over members `jobsReady` does not count and a reader pairing them is comparing two sets.
       ONE-SIDED ON PURPOSE — a non-zero count with a zero gap is the ordinary reading that the backlog's best
       claim stands AT the front, which is the row's whole point and must not fire. */
    DCHECK(out->jobs_ready > 0 || out->job_w_gap == 0.0,
           "the WFQ census states how far the job backlog stands behind the front of the order while counting "
           "no job that waits on rank at all — the gap is written only for a holder the ready arm admitted, so "
           "a distance with no population under it means the two rows have stopped being about one set and "
           "`jobsReady: 0` no longer makes `jobWGap: 0`");
    /* AND THE TWO KINDS OF READY JOB ADD UP TO THE READY COUNT — the identity that makes the pair a PARTITION
       rather than two counts standing beside a total, asserted here because this is where all three are in one
       hand. THAT THE SIDES HAVE DIFFERENT WRITERS IS THE WHOLE CONTENT OF THE CHECK: the total accumulates the
       queue's own `length` and the halves accumulate a walk of its records, so it can fail, and what it fails
       on is an edit that moves one accumulation site and not the other, or a reset that stopped covering one
       half. A reader meeting a split that does not close would be deciding which arm of flow_step is holding a
       backlog from two numbers taken over different sets, which is the one reading the pair exists to make.
       engine/build.mjs asserts the same identity again, for `unframed_picks_lifetime`'s reason: this DCHECK is
       compiled out of a release build that reader still runs over. */
    DCHECK(out->jobs_ready_task + out->jobs_ready_micro == out->jobs_ready,
           "the WFQ census splits the rank-ready job backlog into two kinds that do not add up to it — the "
           "total is the queue's length and the halves are a walk of the queue's own records, so a split that "
           "does not close means the two are no longer one sample, and neither half then says which arm of "
           "flow_step is holding the backlog");
    /* AND THE REPLY BACKLOG'S IS NON-NEGATIVE BY THE SAME CONSTRUCTION AND ASSERTED FOR THE SAME REASON — a
       ready delivery holder is one of the members flow_best's maximum is taken over, read through the same
       flow_weight in the same scan. The row exists to answer whether a delivery backlog is an ORDERING problem,
       which is precisely the reading a wrong sign inverts. */
    DCHECK(out->deliv_w_gap >= 0.0,
           "the WFQ census reports a reply backlog standing ABOVE the front of its own queue — flow_best's "
           "maximum is taken over the same members this scan walks, so a negative deficit means the pick and "
           "the census are no longer reading one comparator, and every conclusion drawn from this row about "
           "whether undelivered replies are outranked or unreachable is a statement about the instrument");
    /* …AND THE SAME PAIRING FOR THE REPLY BACKLOG, COVERING ONE MORE FIELD BECAUSE THE BRANCH WRITES THREE.
       `deliv_w_gap` and the two visit counts at its ends are written in ONE branch guarded by
       `have_deliv_holder`, which the ready arm alone raises. The zeroing of the three states that consequence
       in words already — a reader holding one of them set and the others not would be reading a gap about
       members this scan never found — and asserting it is what makes the sentence checkable: they are written
       together or not at all, so any of them speaking with `deliv_ready` at zero is that branch having
       acquired a second writer. */
    /* AND THE DOCUMENT-PROGRESS GAP IS NON-NEGATIVE BY THE SAME CONSTRUCTION, ASSERTED FOR THE SAME REASON
       `never_picked_gap`'s is: `w_top` is flow_best's maximum over EVERY member with no filter, and the member
       this gap is from is one of them, so the front cannot stand behind it. A negative reading is flow_best
       and this walk disagreeing about the frontier or about the comparator — the one thing that would make
       every sentence read off this row a reading of the disagreement rather than of the run. */
    DCHECK(out->cur_deep_w_gap >= 0.0,
           "a WFQ census read a NEGATIVE gap to the deepest member of the document's program table — `w_top` "
           "is the maximum over every member and the member this gap is from is one of them, so the front of "
           "the order cannot be behind it; this is flow_best and the census walk reading two frontiers");
    /* …AND THE POPULATION CANNOT EXCEED THE FRONTIER IT IS DRAWN FROM. Both sides come out of ONE walk over
       `g_flows_n` — `members` is that bound and `cur_deep_live` is a count of trips through it — so this is an
       assertion about the loop having one population and not an arithmetic tautology: a `continue` added above
       the collection, or a second walk folded into this one, breaks it. */
    DCHECK(out->cur_deep_live <= out->members,
           "a WFQ census counted more members standing at the deepest program row than the frontier holds — "
           "both numbers come out of one walk of the registry, so a count above the population is that walk "
           "having been given a second source of members");
    /* …AND THE THREE ARE WRITTEN TOGETHER OR NOT AT ALL, which is the delivery triple's own check one row
       over. A cursor or a gap speaking with NO member standing at it is that branch having come apart, and it
       is the one shape that would make a reader price an order against a population this scan never found.
       THE CONVERSE IS NOT ASSERTED AND MUST NOT BE: a frontier standing wholly at row 0 reports `cur_deep` 0
       with `cur_deep_live` at the whole frontier, which is a fresh document and the ordinary state. */
    DCHECK(out->cur_deep_live > 0 || (out->cur_deep == 0 && out->cur_deep_w_gap == 0.0),
           "a WFQ census stated a deepest program row or a distance to it with NO member standing there — the "
           "cursor, its population and the gap are written in one branch, so any of them speaking alone is "
           "that branch having come apart and the row is a distance to nobody");
    DCHECK(out->deliv_ready > 0 ||
           (out->deliv_w_gap == 0.0 && out->deliv_w_gap_vis == 0 && out->w_top_vis == 0),
           "the WFQ census states a reply backlog's distance from the front of the order, or the visit counts "
           "at its two ends, while counting no reply that waits on rank — all three are written in the one "
           "branch the ready arm opens, so a reading with no population under it means they have stopped "
           "being about the set `delivReady` counts");

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
    /* AND WHETHER THE ORDER IS DECIDING ANYTHING AT ALL, WHICH EVERY ROW ABOVE PRESUPPOSES AND NONE OF THEM
       ASKS. Each of them describes the shape of an order over the members standing NOW; none says how that
       set is CHANGING, and a comparator is only a scheduler over a set something consumes. These two are
       LIFETIME counters (the kind is in flow.h's rows, not in these names) and they are the frontier's
       arrival and departure processes, so `arrivals / picksLifetime` is what the whole census is conditional
       on. See the banner at `g_arrivals` for why `rankChanges` is not this and for what it reads on the one
       document a reader is most likely to take it from. */
    out->arrivals   = g_arrivals;
    out->departures = g_departures;
    /* …AND WHETHER THE ORDER WAS EVER OFFERED ANYTHING TO ORDER BY, which every reward row above presupposes
       and none of them asks. See the banner at `g_credit_calls` for the two states a zero reward band hides
       and for why the dropped arm is correct rather than a defect to repair. */
    out->credit_calls   = g_credit_calls;
    out->credit_paid    = g_credit_paid;
    out->credit_dropped = g_credit_dropped;
    /* THE IDENTITY THAT DEFINES THE PARTITION, ASSERTED AT THE ONE MOMENT ALL THREE ARE IN ONE HAND. The total
       is raised at flow_credit_emit's single entry and each arm on the line that decides it, so this is exact
       rather than approximate; what it catches is a THIRD exit added to that function later — an early return
       past a departed family, a release-mode arm on a non-positive credit — which would move the total while
       leaving both published arms describing a different population than the one they are a partition of. */
    DCHECK(out->credit_calls == out->credit_paid + out->credit_dropped,
           "the credit partition does not add up — flow_credit_emit's arms are supposed to be exactly two and "
           "the total is raised at its one entry, so a third exit has been added that leaves the order's own "
           "ledger unaccounted for, and `creditsDroppedLifetime` is about to be published as the count of "
           "findings no account could be given when it is a count of something narrower");
    /* THE IDENTITY THAT DEFINES THEM, ASSERTED AT THE ONE MOMENT ALL THREE TERMS ARE IN ONE HAND — which is
       what makes them counters rather than numbers, and what makes the pair checkable from OUTSIDE this
       process on the published document. Each has exactly one writer (flow_new's append, flow_remove's
       swap-remove), so this is exact rather than approximate, and a second writer of either — a frontier trim,
       an eviction, a host that reached into `g_flows` — fires HERE instead of leaving a ratio that is quietly
       about a different population than the members it is divided against. */
    DCHECK(g_arrivals - g_departures == (int64_t)out->members,
           "the frontier's arrival and departure counts do not add up to the members standing in it — one of "
           "the two has acquired a second writer, or a member joined or left `g_flows` without going through "
           "flow_new or flow_remove, and `arrivals / picksLifetime` is about to be published as the rate at "
           "which THIS frontier grows when it is a rate about some other population");
    /* THE TWO SPELLINGS OF "UNFRAMED" ARE A CONTAINMENT AND NOT AN IDENTITY, AND THE DIFFERENCE IS WHAT THIS
       ASSERT NOW RESTS ON. It used to rest on the two being the SAME predicate written twice in one loop —
       the job split's third arm reached through `!flow_host_owed(f) && !f->frame`, `mem_unframed` counting
       `f->frame == NULL` — and warned that re-spelling either arm through flow_stack_empty would drift them
       apart. That re-spelling has since happened, deliberately and at the job split, because `frame` alone was
       not the predicate flow_step's job arms are written against. It does NOT drift the pair: flow_stack_empty
       begins `if (f->frame) return 0;`, so every member reaching the ready arm has a null frame and is inside
       `mem_unframed` BY CONSTRUCTION rather than by two writers agreeing. What the assert stopped being able
       to catch is a re-spelling of `mem_unframed`; what it still catches, and is the reading the row is for, is
       a ready count with nothing unframed under it.
       ONE-SIDED ON PURPOSE, and flow.h says why: `mem_unframed > 0` with no ready job is the SECOND silence,
       which is the state this row was added to name, so the converse is not an invariant and asserting it
       would fire on exactly the reading the row is for. */
    DCHECK(out->mem_unframed >= 0 && out->mem_unframed <= (long)out->members,
           "the count of members holding no frame is outside the frontier it was taken over — it is raised "
           "once per member on the same walk that sets `members`, so a figure outside [0, members] means the "
           "two are no longer one sample and the pair that separates `jobsReady`'s two zeroes is not a pair");
    DCHECK(out->jobs_ready == 0 || out->mem_unframed > 0,
           "this census reports jobs waiting on RANK ALONE while reporting that every member holds a frame — "
           "the ready arm is reached only through flow_stack_empty, whose first line refuses a live frame, so "
           "every member it admits is inside `mem_unframed` by construction and the two cannot disagree "
           "unless `mem_unframed` has been re-spelled. If it has, `jobsReady: 0` can no longer be told from "
           "`mem_unframed: 0`, which is the one reading this row was added for");
    /* …AND THE SAME CONTAINMENT FOR THE REPLY BACKLOG, WHICH HAS NOTHING STANDING UNDER IT IN THE ONE STATE
       THE ASSERT ABOVE IS SILENT IN. Both ready arms are reached only through flow_stack_empty, whose first
       line is `if (f->frame) return 0;`, so a member either arm admits is inside `mem_unframed` BY
       CONSTRUCTION — one question asked at two sites, and only the job site asked it. A frontier holding a
       deliverable reply and no queued job is an ordinary state (`jobsReady: 0` with `delivReady > 0`), and in
       it the job assert cannot fire whatever `mem_unframed` has become, so a re-spelling would reach the
       published rows with nothing anywhere to say that the pair separating `delivReady`'s two zeroes is no
       longer a pair. */
    DCHECK(out->deliv_ready == 0 || out->mem_unframed > 0,
           "this census reports replies waiting on RANK ALONE while reporting that every member holds a "
           "frame — the ready arm is reached only through flow_stack_empty, whose first line refuses a live "
           "frame, so every member it admits is inside `mem_unframed` by construction and the two cannot "
           "disagree unless `mem_unframed` has been re-spelled");
    /* AND THE TWO IDENTITIES THAT DEFINE THE BRANCH ROWS, ASSERTED WHERE BOTH HALVES OF EACH ARE IN ONE HAND.
       They are the only property of a per-bucket number a reader can check without re-deriving the mechanism
       that produced it, and both are exact rather than one-sided because each event has exactly one writer.
       MEMBERSHIP: every live member belongs to exactly ONE bucket (`branch` is set once at flow_new and moved
       once, at the fork, as a matched departure and arrival), and the walk above opens each distinct bucket
       exactly once, so the live counts PARTITION the frontier. A break is a bucket a member no longer names —
       a fork that did not move its membership, a departure that did not surrender it, or a `branch` pointer
       re-written by something that is not flow_fork_inherit — and every branch reading is then about a
       population that is not this frontier. It is one-sided in NEITHER direction on purpose: a sum BELOW
       `members` is a member counted nowhere, and a sum ABOVE it is a member counted twice, and those are two
       different defects that must not share an answer.
       BURN: every microsecond charged in flow_age_running lands on exactly one bucket's `sub_us`, and a bucket
       is freed only once nothing in its subtree is live, at which point acct_unref folds its total into the
       retired term. So the whole of the thread this instance has spent is attributed at every instant. A break
       is a charge that reached one half of flow_age_running and not the other, or a bucket freed without its
       fold — and either leaves `brUsLifeMax / chargedUsLife`, which is the concentration reading these rows
       exist for, as a fraction of the wrong denominator. ALL THREE TERMS ARE PUBLISHED, so this is also
       checkable from outside the process on the emitted document, exactly as the arrival pair above is. */
    /* THE AWAY POPULATION COUNTED TWO WAYS — the identity that makes `away_n` worth maintaining at all, and
       the reason this row is not a counter with nothing under it. The left side is four incremental
       statements at four sites summed over the frontier's distinct families; the right side is this walk
       asking flow_own_silence of every member. They are two maintainers over one instant, so a difference is
       a TRANSITION SITE THE INCREMENTAL SIDE DOES NOT HAVE — a fifth writer of `cpu`, `cpu_gen` or `family`
       that moves a member across its family's base without telling the count — and never a disagreement
       about what the population is.
       IT IS THE ONE CHECK THAT CAN CATCH AN OMISSION RATHER THAN A MISCOUNT, which is why it is asserted and
       not merely published: the four sites were enumerated by reading every writer of those three fields,
       and an enumeration is exactly the artifact this project rates as unable to state its own completeness.
       BOTH SIDES ARE PUBLISHED, so this is also checkable from outside the process on the emitted document,
       exactly as the branch partition below it is. */
    DCHECK(out->epoch_away_live == out->epoch_away_walk,
           "the frontier's maintained away-from-base count disagrees with a direct reading of it — `away_n` "
           "is raised at a charge that takes the running member off its family's base, at a fork whose "
           "newborn inherits a burn, and lowered at a departure, and zeroed for a whole family at "
           "flow_credit_emit's bump with the emitter alone put back. A difference is a FIFTH site moving a "
           "member across that base without maintaining the count: a writer of `Flow.cpu`, of `Flow.cpu_gen` "
           "or of `Flow.family` that this enumeration does not name, or one of the four raising the count on "
           "the wrong family — acct_depart in particular must read BEFORE it clears either pointer, because "
           "flow_own_silence answers zero for a member whose family is gone. `epochRebuildLifetime` is a sum "
           "of exactly this quantity taken at every emission, so it is wrong by however much this is");
    DCHECK(out->br_live_sum == (long)out->members,
           "the frontier's branch buckets do not partition its members — a live member belongs to exactly one "
           "bucket and each bucket is opened once per census, so a sum below `members` is a member counted in "
           "no bucket and a sum above it is one counted in two. Every per-branch row is about to be published "
           "as a share of a frontier it is not a partition of");
    /* AND A FRONTIER WITH MEMBERS STANDING HAS A THINNEST BUCKET SOMEBODY IS STANDING IN — the invariant that
       makes wfq_accounted_spread's branch guard dead code for a live frontier rather than the silent
       term-deleter it was. Every live member's own bucket is taken by the member door with a live count of at
       least one (the member was minted into it and has not departed), so a census reporting members cannot
       leave the live minimum at zero. It is NOT the partition identity above restated: that one is satisfied
       by any set of buckets whose live counts sum correctly, EMPTY ONES INCLUDED, which is exactly how the
       defect this replaces stayed invisible through 56 censuses of an assert that held every time. */
    DCHECK(out->members == 0 || out->br_live_min > 0,
           "the census reports members standing and no branch bucket holding one — every live member is "
           "minted into a bucket and reaches it through `branch`, so a zero here is the live extrema ranging "
           "over a bucket NOBODY stands in, and wfq_accounted_spread is about to score the branch term's "
           "whole range as 0.0 while the order spans it");
    /* AND THE MINT PAIR AGAINST THE LIVE PAIR IT IS FOLDED BESIDE — the relation that says the two are over
       ONE population, which is the only thing that makes `1/br_born_min - 1/br_born_max` a RANGE rather than
       two numbers subtracted. `sub_born = live + sub_gone` with `sub_gone >= 0`, so on any single bucket the
       mint count dominates the live count; take both extrema over the SAME set of buckets and each end of the
       mint pair dominates its opposite number.
       IT IS NOT VACUOUS AND IT IS NOT THE PARTITION IDENTITY RESTATED. The two pairs are written by two
       statements inside branch_take, and nothing but a shared guard keeps their populations equal — fold the
       minimum over every taken bucket instead and a departed root's single-member bucket drags `br_born_min`
       to 1 beneath a `br_live_min` of 2, which fires here and is exactly the defect `br_live_min` already
       cost this file 48 censuses to find. The partition identity above is satisfied by any set of buckets
       whose live counts sum correctly and says nothing whatever about which buckets an extremum ranged over. */
    DCHECK(out->br_born_min >= out->br_live_min && out->br_born_max >= out->br_live_max,
           "a branch bucket's lifetime mint count is below its own live membership — `sub_born` is `live` "
           "plus everything that has ever departed the bucket, so this is the mint extrema and the live "
           "extrema ranging over two different sets of buckets, and the branch term's range is about to be "
           "published as a difference between two numbers that are not two ends of one reading");
    /* AND THE CROWD TRIPLE AGAINST THE ROW IT IS SELECTED BY — the identity that says these three describe
       ONE bucket rather than three. The two sides are written by two writers at two instants: the left is a
       dereference of the node branch_take retained, performed after the walk ended, and the right is a
       running maximum folded over every live bucket during it. They agree only while the fold that moves the
       maximum is the same statement that moves the pointer, which is exactly the property a reader cannot
       check on the emitted document any other way and exactly the one an edit would break silently.
       IT IS NOT VACUOUS: writing the three rows at the maximum's own assignment would make this a comparison
       of one statement with itself, and this file refuses that form everywhere else. */
    DCHECK(out->br_crowd_live == out->br_live_max,
           "the branch bucket the census retained as the frontier's fattest does not hold the live maximum "
           "the same walk folded — the pointer and the maximum are moved by one statement, so a disagreement "
           "means one of them is now moved by another, and the crowd's mint count and receipt are about to be "
           "published as the fattest arm's when they are some other bucket's");
    /* AND THAT BUCKET INSIDE THE POPULATION THE OTHER TWO EXTREMA RANGED OVER, which is a different statement
       from the one above and catches a different break: the identity above says the retained node owns the
       live maximum, and these say it went through the SAME folds the mint and burn extrema were taken from.
       A bucket reached by one door and not the others — a guard that came apart, a door added without its
       folds — leaves a crowd whose own numbers sit outside the range the rows beside them publish, and every
       coincidence reading (`is the crowd also the hungriest arm', `is it also the one that has taken most
       arms') would then be comparing two populations. Both are one-sided because a maximum is an upper bound
       and the crowd is one member of the set it is taken over. */
    /* AND THE MINTER TRIPLE AGAINST THE ROW IT IS SELECTED BY — the same form as the crowd's identity above
       and a statement about a different pointer, so neither catches the other's break. The two sides are
       written by two writers at two instants: the left is one dereference of the node branch_take retained,
       performed after the walk ended, and the right is a running maximum folded over every live bucket during
       it. They agree only while the fold that moves the maximum is the same statement that moves the pointer.
       IT IS WRITTEN AS A SUM AND NOT AS A SECOND `born` FIELD, which is what keeps it from being vacuous: the
       two terms are the node's live count and its shed count, read as `sub_born - sub_gone` and `sub_gone`,
       so their sum reaches `sub_born` by a path the fold never took. A fourth published row holding that
       bucket's own `sub_born` would have made this a comparison of one load with another of the same field,
       and `br_born_max` is already that number for the bucket this names.
       AND IT IS THE ONE CHECK ON `br_minter_gone`, which nothing else in this struct can bound: the shed
       count has no extremum beside it, so without this identity it would be a number with a paragraph
       attached rather than a counter with a definition. */
    DCHECK(out->br_minter_live + out->br_minter_gone == out->br_born_max,
           "the branch bucket the census retained as the frontier's biggest MINTER does not hold the mint "
           "maximum the same walk folded — its live count plus what it has shed IS its lifetime mint count, "
           "so a disagreement means the pointer and the maximum are no longer moved by one statement, and the "
           "shed count and receipt of the arm that has taken most arms are about to be published as some "
           "other bucket's");
    /* AND THAT BUCKET INSIDE THE POPULATIONS THE OTHER ROWS RANGED OVER, which is a different statement from
       the one above and catches a different break, exactly as the crowd's pair does: the identity above says
       the retained node owns the mint maximum, and these say it went through the SAME folds the live extremum
       and the held-burn accumulator were taken from. A bucket reached by one door and not the others leaves a
       minter whose own numbers sit outside the totals the rows beside them publish, and `br_minter_us /
       br_held_us` — the reading the whole triple exists for — would then be a fraction of a denominator its
       numerator is not drawn from. Both are one-sided because a maximum and a sum are upper bounds over sets
       the minter belongs to. */
    DCHECK(out->br_minter_live <= out->br_live_max && out->br_minter_us <= out->br_held_us,
           "the biggest-minting live branch bucket holds more members than the live maximum, or more thread "
           "time than the live buckets hold between them — an extremum and a sum are upper bounds over a set "
           "this bucket belongs to, so this is the minter being reached through a door the live fold or the "
           "held-burn accumulator is not under, and its share of the thread is about to be published against "
           "a population it is not part of");
    DCHECK(out->br_crowd_born <= out->br_born_max && out->br_crowd_us <= out->br_us_max,
           "the fattest live branch bucket's own mint count or receipt is above the maximum the census took "
           "across the buckets it reached — an extremum is an upper bound over a set the crowd belongs to, so "
           "this is the crowd being reached through a door the extrema are not folded under, and the "
           "coincidence readings the crowd rows exist for are about to compare two different populations");
    /* AND THE BURN SPLIT, WHICH IS THE DENOMINATOR HALF. Two accumulators in the two arms of branch_take's
       one condition, so this fails if either arm stops firing or if the live guard comes apart from the one
       the live extrema are folded under. Chained with the identity directly below it, every microsecond the
       scheduler has charged lands in exactly one of three published places — a bucket somebody stands in, a
       bucket still taken and standing empty, or a bucket whose subtree has wholly departed — and that total
       cannot move without one of its three parts moving. All four terms are published, so the chain is
       checkable from outside the process on a release document where this is compiled out. */
    DCHECK(out->br_held_us + out->br_empty_us == out->br_us_sum,
           "the thread time held by live branch buckets and by empty ones does not add up to the thread time "
           "attributed to buckets at all — the two are raised in the two arms of one condition in "
           "branch_take, so a break is an arm that stopped firing or a guard that no longer decides both, and "
           "the crowd's share of the thread is about to be published against a denominator drawn from a "
           "different set of buckets than its numerator");
    DCHECK(out->br_us_sum + out->br_retired_us == out->charged_us,
           "the thread time attributed to branch buckets does not add up to the thread time the scheduler has "
           "charged — every microsecond lands on exactly one bucket and a bucket's total is folded into the "
           "retired term when it is freed, so a break is a charge that reached the aging and not the bucket, "
           "or a bucket freed without its fold, and `brUsLifeMax / chargedUsLife` is about to be published as "
           "a fraction of a denominator the numerator is not drawn from");
    /* AND THE FAN AGAINST THE BUCKET THAT HOLDS IT — the relation that makes `br_fan_max / br_fan_sum` a
       reading INSIDE the buckets above rather than a second population beside them. Every member the fan
       counts sits under a NON-ROOT parent, and a fork off a non-root joins its parent's bucket, so all of one
       fan's members are live members of ONE bucket and the widest fan cannot exceed the fattest bucket.
       IT IS NOT THE PARTITION IDENTITY RESTATED AND IT IS NOT VACUOUS, which is the test this file applies to
       every assertion it adds. The two sides come from different writers: the fan is counted by the member
       walk above, and `br_live_max` is `sub_born - sub_gone` maintained at flow_fork_inherit and acct_depart.
       A departure that missed its `sub_gone`, or a `branch` re-pointed by anything but the fork, separates
       them. A `br_fan_sum + <members with a root parent> == members` identity was deliberately NOT written
       instead: both terms would be raised once per trip of this scan's own loop, so it is a loop counter
       compared with itself — a non-check wearing the syntax of one, which is worse than no check at all. */
    DCHECK(out->br_fan_max <= out->br_live_max,
           "a fork's live children outnumber the whole branch bucket they were forked into — a fork off a "
           "non-root joins its parent's bucket, so every member counted in a fan is a live member of one "
           "bucket and this says the two are counting different frontiers: either a departure left a bucket's "
           "`sub_gone` unraised, or a member's `branch` was moved by something that is not the fork");
    /* AND THE DEPTH AGAINST THE FAN IT BELONGS TO, because a `br_fan_depth` of 0 is how a reader is told
       there is NO fan and a real fan can never stand at one: its node has a parent (the door tests `up->up`),
       and a node with a parent went through flow_fork_inherit, which wrote `depth = parent->depth + 1`. The
       two fields are written by two statements and the compression moves `up` while explicitly leaving
       `depth` alone, so they can disagree — which is the whole reason this is asked rather than assumed. It
       is NOT the pair above restated: that one relates the fan to the BUCKET, this one relates the published
       depth to the fan's own existence, and a reader who took a 0 for "no deep fork" would be wrong. */
    DCHECK(out->br_fan_max == 0 || out->br_fan_depth > 0,
           "the census reports a fork below the top level standing at fork-tree depth 0 — depth 0 is a family "
           "ROOT and a root is excluded from the fan by the same predicate that decides a branch bucket, so "
           "either `up` names a node no fork attached or `depth` was never written beside it, and "
           "`brFanDepth: 0` is about to be published as the statement that this frontier has no deep fork");
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
           "frees that Array and every [vector, taken, arm] row it names, so a flow reaching here with one was "
           "removed without being released");
    DCHECK(f->dyn == NULL && f->dyn_cand == NULL && f->dyn_type == NULL && f->dyn_url == NULL &&
           f->dyn_el == NULL && f->dyn_doc == NULL && f->dyn_id == NULL && f->dyn_run == NULL &&
           f->dyn_token == NULL && f->dyn_pos == NULL && f->dec_blob == NULL && f->pin_blob == NULL,
           "a flow was removed with its lazily-loaded chunk bodies or its suspended decision/pin blobs still "
           "attached — the flow's own allocations, freed by nothing else");
    /* THE SLOT IS READ OFF THE MEMBER AND THE WALK THAT USED TO FIND IT IS GONE (flow.h's `reg_i`). A
       departure was one scan of every member standing, on a frontier that reaches thousands and whose
       departures are as many as its forks — so removing was quadratic in the frontier over a run, for a
       question that is not about any other member. This changes NOTHING about the order: the frontier's array
       is arrival order, the queue is weight order, and no comparison anywhere reads a slot.
       THE GUARD IS A `CHECK` AND NOT A `DCHECK`, AND THIS DIFF IS WHAT PROMOTED IT. The scan it replaces was
       an address comparison, so in RELEASE a stale or double-removed pointer simply found nothing and this
       function returned having done nothing; the handle makes `f->reg_i` decide which slot gets overwritten,
       which is a WRITE into the registry in every build. §Offensive programming's rule is that a diff making
       a checked value load-bearing in release carries the promotion with it, and the invariant here is data
       integrity — a swap-remove at a slot that does not hold this flow drops whichever member does hold it,
       and §scheduler's razor makes a dropped flow a cap. Both sides are read from live storage and neither
       can be optimised away, so this cannot be the assert whose two sides agree by construction.
       IT SUPERSEDES THE `DFAIL` THAT STOOD BELOW THE WALK rather than standing beside it: that abort said
       "double remove or a dangling Flow*" and fired only in dev, and this says the same thing at the same
       states in every build. Keeping both would be the weaker copy running in the build that ships.
       AND IT READS ONE INT THROUGH `f` WHERE THE WALK READ NONE, WHICH IS A REAL TRADE AND IS STATED RATHER
       THAN GLOSSED. The scan compared addresses, so on a dangling pointer it dereferenced nothing — and what
       it then DID was return having done nothing at all: no departure counted, the `arrivals - departures ==
       members` identity silently false, and the caller believing a flow left the frontier that is still in
       it. This reads `f->reg_i` first, so a genuinely freed pointer is a read of freed memory; every outcome
       of that read but one aborts here by name, and the one that would not — the array still holding this
       exact pointer at that slot — cannot arise, because the removal that freed it overwrote its slot. A
       loud abort on a state that was silently corrupting the registry is the trade §Offensive programming
       asks for, and under the Asan §Architecture prescribes for memory crashes the read itself is the
       report. */
    {
        int slot = f->reg_i;
        CHECK(slot >= 0 && slot < g_flows_n && g_flows[slot] == f,
              "engine: a flow was removed from the frontier at a registry slot that does not hold it — this "
              "is a double remove, a dangling Flow*, or a member whose slot was not re-keyed by the swap "
              "that moved it. Proceeding would swap-remove whichever member IS at that slot, dropping a work "
              "item the WFQ may never drop");
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
        /* AND THE DISPATCHES IT HOLDS, MOVED OUT OF THE GAUGE AND INTO THE COUNTER ON THE ONE LINE THAT CAN
           STILL READ THEM. `f->picks` stops being reachable two statements below, so this may not stand beside
           the `g_departures++` that counts the same event — see the banner at `g_picks_departed` for why that
           is the identity's own tell rather than an untidiness, and for what the identity is. */
        g_picks_departed += f->picks;
        acct_depart(f);
        free(f);
        g_flows[slot] = g_flows[--g_flows_n];   /* swap-remove; order is by weight, not position */
        /* …AND THE ONE MEMBER THE SWAP MOVED, RE-KEYED ON THE LINE THAT MOVED IT. A departure renumbers
           EXACTLY ONE other member and this is it; the guard is `slot < g_flows_n` and not a comparison of
           pointers because the decrement above has already run, so removing the LAST member self-assigns and
           there is nobody to re-key — writing `g_flows[slot]->reg_i` unguarded would then dereference the
           slot one past the end, which the flow just freed used to occupy. */
        if (slot < g_flows_n) g_flows[slot]->reg_i = slot;
        /* …AND THE DEPARTURE COUNTED AS ONE, on the line that performs it. This decrement and the
           registry coming up are the only two ways `g_flows_n` falls, and the second is asserted to
           happen only on an already-drained frontier, so the pair's conservation identity is exact. */
        g_departures++;
        frontier_rank_changed();   /* frontier changed */
    }
}

int flow_count(void) { return g_flows_n; }

/* IS THIS POINTER ONE OF THE FLOWS STANDING? — AND IT MAY NOT ANSWER THROUGH flow.h's `reg_i`, WHICH IS THE
   WHOLE REASON THIS WALK SURVIVES A DIFF THAT DELETED ITS TWIN AT flow_remove.
   THE TWO CALLERS ASK DIFFERENT QUESTIONS OF ONE PREDICATE AND THE STRICTER ONE DECIDES IT. flow_pick asks
   "is my seed still a member", of a pointer it believes is live; engine.c's hook and this file's two guards
   ask "is this pointer DANGLING" — `g_rival` is cached across opcodes and its own comment says the cached
   rival goes stale, and flow_remove FREES the flow, so the pointer under those asserts may name memory that
   is gone. An address comparison never dereferences `f` and so answers correctly for both; the handle would
   have to LOAD `f->reg_i` to answer either, which reads freed memory in exactly the state the strict question
   exists to catch. So the guard would be destroyed by making it cheap, and that is a §A-PREDICATE-THAT-ANSWERS
   -TWO-QUESTIONS split to make with a second entry when a caller is known live — never a conversion of this
   one.
   AND THE WALK PAYS FOR ITSELF BY CHECKING THE HANDLE. It has the linear answer in hand at no extra cost, so
   it re-derives `reg_i` over the whole frontier at every call. It is two-sided (the field against the array,
   both read from live storage) and it is placed AFTER the address match, so it dereferences `f` only once the
   walk has PROVEN the pointer live — the assert is exactly as dangling-safe as the function holding it.
   THE RESIDUAL THAT STOOD HERE IS RETIRED, AND ITS REMEDY CLAUSE WAS WRONG, WHICH IS THE HALF WORTH KEEPING.
   It said flow_pick's seed test "IS STILL A FULL WALK, in every build, once per dispatch", which was true and
   is no longer: the pick decides membership inside the scan it was already performing (flow_pick's own
   `seed_live`), so the second walk is gone rather than made cheap. What it named as the next diff — "making
   the pick's own membership question O(1)" — and what it named as the precondition — "a statement that
   flow_pick's `seed` is LIVE" — were BOTH unnecessary, and both for one reason: the clause pictured this
   function's HANDLE being read at the pick, which really would need the pointer live, instead of the answer
   already passing through the pick's own loop. The clause was checkable against the tree the day it was
   written and nobody checked it, because a remedy clause is read once, by somebody who has already decided to
   do the work. ITS OTHER TWO CLAUSES HELD EXACTLY: the walk was unpriced (`g_scan_weights` counts weight
   EVALUATIONS and this one evaluates none), so the frontier-weighing rows did understate the dispatch pick by
   one whole walk, and a reader differencing `scanNextWeights` across that change is comparing a cost that
   halved against a counter that did not move.
   THE CROSS-CHECK IT PAID FOR DID NOT GO WITH IT, AND IT GOT WIDER. flow.h's `reg_i` rests on this relation
   being re-derived over the frontier at every pick, which this function supplied for exactly ONE member per
   pick — whichever one matched the seed. flow_pick now asserts it for EVERY member of EVERY scan, so the
   claim flow.h makes is stronger than it was and is made where the walk actually happens. */
int flow_is_member(const Flow *f) {
    for (int i = 0; i < g_flows_n; i++) if (g_flows[i] == f) {
        DCHECK(f->reg_i == i,
               "a flow's registry handle names a slot the registry does not hold it at — the frontier gains a "
               "member on exactly one line and loses one on exactly one line, so the two writers of this "
               "relation have come apart, and flow_remove will swap-remove at the handle's slot and drop "
               "whichever member is standing there");
        return 1;
    }
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

/* WHICH ROW OF `f`'s SEQUENCE IS CALLED `id` — see flow.h's declaration for why it asserts nothing.
   A LINEAR WALK IS THE RIGHT SHAPE HERE AND NOT A CONCESSION. It is asked once per external script REPLY and
   once per entry of a destroy walk, against a column whose length is the number of programs one document
   shipped; an index beside it would be a second structure holding row identity, which is the thing that had to
   be got right in ONE place for the register to stop holding positions at all.
   ZERO IS NO ROW, never row zero: `dyn_id` is minted from a counter that starts at 1, so the register's own
   default (solver/pending.h) is a name that cannot resolve, and an entry nobody wrote a row onto answers -1
   here instead of silently naming the sequence's first program. */
int flow_dyn_row_by_id(const Flow *f, uint64_t id) {
    DCHECK(f != NULL, "a flow's script sequence was searched for a row name with no flow to search");
    for (int k = 0; k < f->dyn_n; k++) if (f->dyn_id[k] == id) return k;
    return -1;
}

/* IS ROW `k` ONE HTML §7.5.10 "Destroying documents"' STEP 5 TAKES — the criterion, in ONE place, because the
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

/* See flow.h. HTML §7.5.10 STEP 5 PERFORMED ON THE ONE TASK QUEUE `JS_DropJobsForContext` CANNOT REACH — this
   flow's own program sequence. The rows go, the nine columns shrink together, and every index anything still
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

    DCHECK(f != NULL, "§7.5.10 step 5's program removal was asked of no flow");
    DCHECK(doc != 0, "§7.5.10 step 5's program removal was asked about the NONE document — a handle is this "
                     "instance's index into its own name table and zero names nothing");
    /* THROUGH THE COUNT, so the removal cannot run over a cursor pair the count would have refused: that
       function asserts `script_i >= last_compiled`, which every index below is derived from. */
    removed = flow_programs_unstarted_for_document(f, doc);
    if (removed == 0) return 0;
    for (int k = 0; k < f->dyn_n; k++)
        if (prog_row_removed_by_destroy(f, k, doc)) { lowest = k; break; }
    DCHECK(lowest >= 0,
           "§7.5.10 step 5's removal counted rows to take and then could not find one — the count and this "
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
           "§7.5.10 step 5's removal is about to take a row at or below this flow's cursor — `script_i` and "
           "`last_compiled` are ABSOLUTE positions this walk deliberately does not repair, and they are only "
           "safe because every removed row sits above BOTH. A row taken below either one leaves the cursor "
           "naming a different program than the one it stood at, with every index still in range and nothing "
           "anywhere to say so. If the criterion is meant to reach these rows, the cursor pair has to be "
           "brought down with them here, exactly as `imm_at`/`imm_next` are below");

    /* THE ENTRIES THAT NAME A ROW, FIRST. engine_pending_docscript records the row's NAME on the register
       (PEND_SCRIPT_ROW) and flow_deliver_one_reply finds the row by it, so the register is the one structure
       outside this column that names a row at all.
       AN ENTRY WHOSE ROW IS GOING LEAVES WITH IT, and that is what §7.5.10 step 2's "Abort document." reaches
       — HTML §7.5.11 "Aborting a document load", whose own step 2 is "Cancel any instances of the fetch
       algorithm in the context of document, discarding any tasks queued for them, and discarding any further
       data received from the network for them". The reply has nowhere to land once the row is gone, and a
       reply that DID land would be a program of a destroyed Document compiled into a realm whose browsing
       context is null, which is the exact thing step 5 exists to prevent. Walked BACKWARDS because
       pending_remove is a swap-remove: it moves the LAST entry into the hole, and descending means that entry
       has already been visited.
       AN ENTRY WHOSE ROW SURVIVES BUT SHIFTS NEEDS NOTHING DOING TO IT, AND AN ABORT USED TO STAND HERE
       BECAUSE IT DID. While the entry held the row's absolute POSITION, a removal below it renamed its
       referent, and the position could not be corrected: bringing it down is a WRITE to a record a forked
       sibling shares (pending.h: a fork shares records, and solver/pending_index.h states it outright — one
       record is one member however many flows name it), and that sibling has not destroyed this document and
       has not moved a row, so the one field would have had to hold two different positions at once.
       `pending_unshare` was no way out either: its contract admits a copy only when no reply is coming for the
       original, and an outstanding document script is precisely a record a reply IS coming for. The entry names
       the row by `dyn_id` now — the same name in every arm, unchanged by any shift — so the compaction below
       carries it and there is nothing left to correct or to refuse. */
    for (int k = pending_count(f->pending) - 1; k >= 0; k--) {
        JSValue e = pending_entry(f->pending, k);
        int docscript = (int)pending_get_int(e, PEND_KIND) == FLOW_PENDING_DOCSCRIPT;
        uint64_t rowid = (uint64_t)pending_get_int(e, PEND_SCRIPT_ROW);
        int slot;

        JS_FreeValue(pending_ctx(), e);
        if (!docscript) continue;
        slot = flow_dyn_row_by_id(f, rowid);
        DCHECK(slot >= 0,
               "an external document script's park names a row this flow does not hold — a name is minted at "
               "the row's creation, copied by a fork and carried by this compaction, and only a removal HERE "
               "retires one, so an entry that resolves to nothing outlived its row without this walk taking "
               "it: the park and the column have come apart");
        if (prog_row_removed_by_destroy(f, slot, doc)) { pending_remove(&f->pending, k); continue; }
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
                   "§7.5.10 step 5 removed a destroyed Document's program that still OWED A CROSS-AGENT "
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
            /* THE NAME TRAVELS WITH THE ROW, which is the whole of why this walk no longer aborts: the row
               moves, its `dyn_id` does not change, and every register entry naming it is still naming it. */
            f->dyn_id[w]    = f->dyn_id[k];
            /* AND SO DOES THE ARRIVAL STAMP, by a sentence the name's does not make: a compaction moves a row
               within one flow's sequence and changes nothing about WHEN it became runnable, so a row that is
               older than a queued callback before §7.5.10 step 5 is older than it afterwards. Re-minting here
               would let a destroy reorder a surviving row against this flow's own queue. */
            f->dyn_run[w]   = f->dyn_run[k];
            f->dyn_token[w] = f->dyn_token[k];
            f->dyn_pos[w]   = f->dyn_pos[k];
        }
        w++;
    }
    DCHECK(w == f->dyn_n - removed,
           "§7.5.10 step 5's compaction kept a different number of rows than the count said it would take — "
           "the count and the walk ask one predicate over one column, so a disagreement means the sequence "
           "was written between them and the ten columns no longer describe one queue");
    f->dyn_n = w;
    /* `script_i` AND `last_compiled` ARE DELIBERATELY UNTOUCHED — the proof is the `lowest` assert above, which
       is where it is checkable: there the columns still hold the positions the claim is about. */
    DCHECK(flow_programs_unstarted_for_document(f, doc) == 0,
           "§7.5.10 step 5 removed a destroyed Document's queued programs and the document still has some — "
           "the compaction is over the same predicate the count is, so a survivor means a row moved ACROSS the "
           "cursor and became unstarted, which nothing in this walk can do");
    return removed;
}

/* WHETHER A REGISTER ENTRY IS A FETCH ISSUED IN THE CONTEXT OF `doc` — §7.5.11 step 2's population. Every park
   a document's code or algorithm makes records the asking document (PEND_DOC). A document script's park names
   its row instead and leaves with it (above); a host request is a cross-instance read, not a fetch. */
static int pend_entry_fetch_of_document(JSValueConst e, uint32_t doc) {
    int kind = (int)pending_get_int(e, PEND_KIND);
    uint32_t edoc = (uint32_t)pending_get_int(e, PEND_DOC);

    if (kind == FLOW_PENDING_DOCSCRIPT || kind == FLOW_PENDING_HOSTREQ) return 0;
    DCHECK(edoc != 0,
           "a fetch on a flow's register names no asking document — every fetch park records "
           "document_doc at its push, so this record was pushed by a park that does not");
    return edoc == doc;
}

/* See flow.h. */
int flow_fetches_for_document(const Flow *f, uint32_t doc) {
    int n = 0;

    DCHECK(f != NULL && doc != 0, "the fetch count was asked of no flow or about the NONE document");
    for (int k = 0; k < pending_count(f->pending); k++) {
        JSValue e = pending_entry(f->pending, k);

        if (pend_entry_fetch_of_document(e, doc)) n++;
        JS_FreeValue(pending_ctx(), e);
    }
    return n;
}

/* See flow.h. The register is walked backwards because pending_remove is a swap-remove. A removed entry's
   record survives for any sibling register that names it (pending_index_unref), and a reply already received
   for it is discarded with it, which is step 2's "discarding any further data received from the network". */
void flow_abort_document_load(Flow *f, uint32_t doc) {
    DCHECK(f != NULL && doc != 0, "§7.5.11 was run for no flow or for the NONE document");
    flow_programs_remove_for_document(f, doc);
    for (int k = pending_count(f->pending) - 1; k >= 0; k--) {
        JSValue e = pending_entry(f->pending, k);
        int go = pend_entry_fetch_of_document(e, doc);

        JS_FreeValue(pending_ctx(), e);
        if (go) pending_remove(&f->pending, k);
    }
    DCHECK(flow_programs_unstarted_for_document(f, doc) == 0 && flow_fetches_for_document(f, doc) == 0,
           "§7.5.11 step 2 ran and this flow still holds an unstarted program or a fetch of the aborted "
           "document — both walks above ask the same predicates the counts do");
}
