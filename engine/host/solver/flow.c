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
               "a fork-tree node was freed while the flow it accounts for is still in the frontier — that flow holds "
               "a reference of its own, so the reference was dropped twice");
        /* A branch bucket's totals outlive the bucket, folded here so the census's burn identity stays exact: a
           node with `branch == self` reaching zero means nothing in its subtree is live. This is also where
           `branch` is shown not to dangle: every live member's `branch` names a node on its own ancestry path,
           which acct_compress_dead keeps, so a bucket freed here has no live member naming it. */
        DCHECK(a->branch != NULL,
               "a fork-tree node reached its free with no branch bucket — flow_new, the only mint, points every node "
               "at one, so this node was built somewhere else");
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
           "the frontier's clock was asked to follow a flow that owns no account — a departed flow cannot be the "
           "item in service");
    DCHECK(f->family->placed,
           "the frontier's clock was asked to follow an account that still reads the clock — the dispatch must stamp "
           "the account before installing the flow, as flow_set_running does");
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
    DCHECK(acct_vt_leads(), "an account stands at a reward above the frontier's clock — the clock must be the "
                            "maximum coordinate any account stands at, so a site wrote a reward without serving the "
                            "clock");
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
           "an account that has never held the thread was credited an emission — only the running flow is credited "
           "and a dispatch places its account, so a detector credited a flow not in service or an account was "
           "un-placed");
    return (f->family->placed ? f->family->base : frontier_vt()) + f->family->earned;
}

double flow_reward(const Flow *f) {
    DCHECK(f != NULL, "the WFQ's reward term was asked of no flow");
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
           "a rebuilt flow's parked reward was written through a fork family it does not own — a rebuilt flow founds "
           "its own account, and writing through a joined tag would re-place every arm of another family");
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
               "freezing the frontier's clock into an account's tag moved the flow's queue coordinate — an unplaced "
               "account's reward is that clock, so the stamp must leave the weight the pick compared unchanged");
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
    DCHECK(v > 0.0, "a flow was credited a non-positive emission — the reward term counts new @H/@S findings at one "
                    "point each, and a zero credit would reset the aging without adding reward");
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
           "the flow holding the thread belongs to no fork family — a departed flow was left running, and this "
           "emission has no account to be credited to");
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
           "an @S candidate's runway position was recorded against no flow — the fraction is one flow's progress "
           "along its own recorded path");
    DCHECK(f->cand_payload != NULL,
           "a runway position was recorded for a flow carrying no payload — the runway approaches a candidate's own "
           "source read, and decide.c asks it only of a candidate");
    DCHECK(total > 0 && consumed >= 0 && consumed <= total,
           "an @S candidate's runway position is outside [0, total] — `consumed` is the arms this run replayed and "
           "`total` the recorded path they are a prefix of, so the two came from different flows");
    /* Past the delivery this rung is pinned and may not be written, the ladder's own rule: the source read is
       behind this flow, and a live fraction would rank a delivered candidate whose recorded path runs on past its
       source read below an undelivered one that consumed all of its own. Asserted rather than skipped, because
       reaching here in that state means dec_replay still consumes arms for a flow whose delivery is recorded. */
    if (f->cand_rung >= FLOW_RUNG_DELIVERED) {
        DCHECK(f->cand_replay == 1.0,
               "a delivered @S candidate's runway fraction is not pinned at 1.0 — flow_observe_rung pins it at the "
               "delivery, so the pin was missed and the ladder's bottom two bands overlap");
        return;
    }
    /* The triple describes one sample, asserted on the way in. Below the delivery the fraction is exactly the
       division of the pair beside it, so a disagreement is two writers reaching these fields separately and the
       report built on the pair would contradict the comparator. Asserted after the pin's early return, because
       past the delivery the fraction is a policy and the pair the last observation, which may differ (see the
       field). */
    DCHECK((f->cand_replay_of == 0) == (f->cand_replay == 0.0),
           "an @S candidate's runway fraction and its arm pair disagree about whether a reading was ever taken — `of "
           "== 0` and a zero fraction both mean never observed, so one was written without the other");
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
           "an @S candidate's surviving fraction was recorded against no flow — the distance is a fact about one "
           "flow's own payload");
    DCHECK(frac >= 0.0 && frac <= 1.0,
           "an @S candidate's surviving fraction is outside [0, 1] — it is the surviving run over the payload's own "
           "length, so it was measured against another denominator");
    DCHECK(f->cand_payload != NULL,
           "a surviving fraction was recorded for a flow carrying no payload — the fraction's denominator is the "
           "payload");
    /* And the rung below this one, the ladder's order asserted from its own side. A payload says the substitution
       was installed, not that it happened, and the run about to be recorded is a run of the payload's bytes in a
       string, which the page's own text matches just as readily because a breakout is punctuation. solve.c's three
       candidate-arm entries return before here unless the substituting component reports it performed; this is the
       same statement one layer down, so a fourth sink class or route cannot forget to ask. It also makes
       flow_distance's `cand_surv + cand_rung` sound: the fraction is arithmetically below the delivery and
       chronologically above it, and this asserts that a nonzero fraction at rung 0 cannot exist. */
    DCHECK(f->cand_rung >= FLOW_RUNG_DELIVERED,
           "an @S survival fraction is being recorded for a flow not observed to deliver its payload — the run "
           "measured would be the page's own bytes coinciding with the candidate's");
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
           "an @S candidate's rung was recorded against no flow — a rung is a fact about one flow's own bytes at one "
           "sink");
    DCHECK(rung == FLOW_RUNG_DELIVERED || rung == FLOW_RUNG_ARRIVED || rung == FLOW_RUNG_ESCAPED,
           "an @S rung outside the ladder was recorded — the rungs are the numerator over FLOW_RUNGS_N, so this "
           "would put the fitness term outside [0, 1]");
    DCHECK(f->cand_payload != NULL,
           "a rung was recorded for a flow carrying no payload — a rung says where this flow's injected bytes stood");
    DCHECK(rung <= f->cand_rung + 1,
           "an @S candidate skipped a rung — its bytes were recorded escaping a context they were never recorded "
           "arriving in, so the two observations are about different strings or the arrival was refused for this "
           "payload");
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
           "an arm contradicting its own example was recorded with no flow running — the contradiction is a fact "
           "about one flow's path");
    g_running->path_forced = 1;
}

int flow_path_driven(const Flow *f) {
    DCHECK(f != NULL, "a flow was asked whether it is this tool's own act, of no flow");
    DCHECK(f->orphan == 0 || f->orphan == 1,
           "a flow's orphan mark is neither set nor clear — engine.c's orphan seed writes it as a constant and a "
           "fork copies it, so any other value is memory this field does not own");
    DCHECK(!f->cand_src || (f->cand_payload && !f->cand_sink != !f->cand_vfy),
           "a candidate session is missing its payload or names neither or both of a sink and a witness key");
    return f->orphan || f->cand_src != NULL;
}

FlowPathMarks flow_path_marks(const Flow *f) {
    FlowPathMarks m = { flow_path_forced(f), flow_path_pinned(f) };
    return m;
}

int flow_path_forced(const Flow *f) {
    DCHECK(f != NULL, "a flow's path was asked whether it stood on a forced arm, of no flow");
    DCHECK(f->path_forced == 0 || f->path_forced == 1,
           "a flow's forced-path mark is neither set nor clear — it is written in exactly one place, as a "
           "constant, so any other value is memory this field does not own");
    return f->path_forced;
}

/* flow.h's field says why it is recorded at the choice of a witness and not at the address that spends it. */
void flow_mark_pinned_value(void) {
    DCHECK(g_running != NULL,
           "a determined witness was recorded with no flow running — the determination is a fact about one flow's "
           "path");
    g_running->path_pinned = 1;
}

int flow_path_pinned(const Flow *f) {
    DCHECK(f != NULL, "a flow's path was asked whether it determined a witness, of no flow");
    DCHECK(f->path_pinned == 0 || f->path_pinned == 1,
           "a flow's determined-witness mark is neither set nor clear — it is written in exactly one place, "
           "as a constant, so any other value is memory this field does not own");
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
           "thread time was charged with no flow running — the scheduler charges the flow it just stepped");
    /* The scheduler's clock is monotonic, asserted where its reading arrives. A negative charge raises the flow's
       weight, so a flow burning the thread would climb to the top and stay; only a non-monotonic or wrapping clock
       at the call site produces one. */
    DCHECK(us >= 0, "a negative thread-time charge reached the WFQ — the aging clock must be monotonic, and a "
                    "negative charge would raise the flow's weight");
    /* The account both halves land on, asserted before either reads it: the own half is read over the account's
       silence window (FlowAcct's `emit_gen`), so the first statement dereferences the family pointer. */
    DCHECK(g_running->family != NULL && g_running->family->up == NULL,
           "the running flow's family tag is not the root of a fork tree — an arm joins its parent's family, so a "
           "tag naming its own node is an arm that founded a family and shed its parent's aging");
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
           "a member's parked continuations were asked for while it holds the thread — its queue is in the runtime "
           "for its turn, not on Flow::parked; build the runtime handle at the pick (engine.c asks JS_HasParkedFlow "
           "of the runtime)");
    return f->parked != NULL;
}

/* A flow completed a unit of work: the optimism term's quantity, a count rather than a clock (flow.h's `visits`
   says what a unit is and why the term may not be thread time). A separate call from flow_age_running though
   charged at the same moment by the same caller, because they measure the two quantities the scheduler names
   separately; a step that burned a quantum inside a program charges the first and not the second. The flow is not
   asserted to be the running one (the finish path can have cleared `g_running`); what is asserted is the property
   that makes the count a trial count. */
void flow_credit_visit(Flow *f) {
    DCHECK(f != NULL, "a completed unit of work was credited to no flow");
    DCHECK(f->frame == NULL,
           "a flow was credited a completed unit of work while still inside a program — a preempt is the middle of a "
           "trial, and crediting it would make the optimism term a second clock");
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
           "a flow was credited a completed unit of work while it still owes a microtask checkpoint — HTML §8.1.4.4 "
           "\"Calling scripts\" step 3 of clean up after running script runs the checkpoint in the turn that is "
           "ending, so the unit is not over");
    /* And the two above are read through the predicate a second consumer shares, so the boundary cannot gain a
       clause in one place without the other. The two specific asserts stay because each names its cause; this one
       pins that they are the whole of flow_between_units' boundary, which the starvation counter relies on. An
       identity under the code as it stands, so it fires only on an edit that breaks it. */
    DCHECK(flow_between_units(f),
           "a completed unit of work was credited to a flow flow_between_units says is inside one, while the two "
           "asserts above passed — the unit boundary gained a clause in one of the two places that state it and not "
           "the other");
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
void flow_fork_inherit(Flow *sib, const Flow *parent, FlowPathMarks at_branch) {
    DCHECK(sib != NULL && parent != NULL && sib != parent,
           "a fork's accounting was inherited from nobody, or from itself");
    /* The marks are monotone, so the parent holds every mark it held at the branch; one it has gained since is
       its own arm's (decide.c marks after the fork takes its blobs) and is not the sibling's. */
    DCHECK(at_branch.forced <= parent->path_forced && at_branch.pinned <= parent->path_pinned &&
           (at_branch.forced == 0 || at_branch.forced == 1) && (at_branch.pinned == 0 || at_branch.pinned == 1),
           "a fork handed its sibling a path mark its parent does not hold — the marks are read off the parent at "
           "the branch and only grow, so this is a mark read off another flow or a bit that is not one");
    /* Nothing may have been credited or charged first, because the inheritance assigns both terms: a sibling run or
       credited before receiving its account ran at a rank nobody chose, and the assignment would erase the result. */
    DCHECK(sib->val == 0.0 && sib->cpu == 0 && sib->cpu_gen == 0 && sib->visits == 0 && sib->picks == 0 &&
           sib->cand_replay == 0.0 && sib->cand_replay_of == 0 && sib->cand_replay_arms == 0 &&
           sib->cand_surv == 0.0 && sib->cand_rung == 0 && sib->path_forced == 0 &&
           sib->path_pinned == 0 &&
           sib->family == sib->acct && sib->family->fam_us == 0 && sib->family->emit_gen == 0 &&
           sib->family->base == 0.0 && sib->family->earned == 0.0 && !sib->family->placed,
           "a forked sibling was credited, charged, dispatched or decided before inheriting its parent's account — "
           "the inheritance assigns every one of these fields, and `picks` must still be zero because no dispatch "
           "may precede it");
    /* And it may not already stand under a flow in the fork tree: flow_new mints a root node, so a non-NULL `up` is
       a second inheritance, leaking the first parent's node and charging every arm it sheds to the wrong chain. */
    DCHECK(sib->acct != NULL && sib->acct->up == NULL,
           "a forked sibling already stands under a flow in the fork tree — flow_new mints a root, so this is a "
           "second inheritance onto one sibling");
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
               "a forked sibling reached its inheritance without the branch bucket flow_new opens, or already "
               "holding membership in one — the transfer below moves exactly one member");
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
       run did not contradict, to be made in the scheduler's rule and not inside a weight term.
       Read at the branch (`at_branch`), not off the parent now: a snapshot fork is assembled after the parent's
       branch has marked its own arm. */
    sib->path_forced = at_branch.forced;
    /* And the witness mark with it, for the same reason: a witness the parent had chosen is one the arm computes
       from too, and a sibling born clean of it would claim its addresses rest on the document's own bytes. Not in
       the equality, for `path_forced`'s reason. */
    sib->path_pinned = at_branch.pinned;
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
           "the running flow was released — its heap delta, document writes and decision state are live rather than "
           "parked");
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
           "a flow was released holding work (a parked continuation, a suspended frame, queued reactions, replies "
           "owed to it, or a routed message) without being written to the cold tier — nothing replays it, so the "
           "frontier just dropped those work items");
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
                "the frontier is gone and %d heap call frame(s) are still live — every frame belongs to some flow's "
                "suspended chain. %d flow(s) were released; the last suspended one whose release freed no frame was "
                "#%d (frame=%d park=%d paged=%d started=%d), and -1 means every suspended flow dropped its chain",
                JS_TrampFrameCount(rt), rel_i, culprit, culprit_frame, culprit_park, culprit_paged,
                culprit_started);
#endif
        DCHECK(JS_StepMachineCount(rt) == 0,
               "the frontier is gone and step machines are still live — a continuation-holding builtin's state "
               "outlived the flow suspended inside it");
    }
    /* And not one node of the fork tree: every reference on a node is its own flow's or a live descendant's, and
       both are gone. This makes acct_compress_dead's re-pointing checkable (a node it re-pointed and failed to
       release holds the ancestry above it, and a node is not a GC object), and it catches a family root kept alive
       by a `family` tag nobody dropped: a departing flow clears its own (acct_depart), so a survivor names the exit
       that did not. */
    DCHECK(g_acct_live == 0,
           "a fork-tree node outlived the frontier — its only references are its flow's and live descendants', all "
           "released by here");
    /* And not one frozen segment. A segment of the heap or document chain is referenced only by the deltas forked
       below it and the segments above it, the world registry's foreign segments and the frontier's flows, both
       released above, so the count must be zero, as decide_free asserts for the third chain on the same primitive.
       A non-zero count is a delta reference nobody dropped, holding no live JSValue and invisible to gc_obj_list.
       This makes cow_delta_release's and dom_base_release's refcount arithmetic checkable. */
    {
        long segs = 0, entries = 0;
        cow_chain_stats(&segs, &entries);
        DCHECK(segs == 0,
               "a frozen heap segment outlived the frontier — every reference on the chain belongs to a delta the "
               "flows and the world registry have released");
        dom_cow_chain_stats(&segs, &entries);
        DCHECK(segs == 0,
               "a frozen document segment outlived the frontier — every reference belongs to a released delta, and "
               "the segment still holds nodes the flows created");
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
               "a frozen constraint segment outlived the frontier — every reference on that chain is a parked flow's "
               "blob or the running flow's globals, both released by here");
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
    f->cand_src = NULL; f->cand_payload = NULL; f->cand_sink = NULL; f->cand_vfy = NULL;
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
           "a from-baseline flow was charged, credited or dispatched before it arrived — the arrival requires every "
           "one of these fields at zero, and an unplaced account");
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
           "a flow arriving from the baseline did not enter at the frontier's virtual time — the arrival rule places "
           "every from-baseline account exactly at the clock");
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

void flow_seed(Flow *f) {
    /* The sequence is established before the flow is ever picked: a member picked with an empty sequence would
       report itself finished, the one answer that is never retried. */
    DCHECK(g_seed_hook != NULL,
           "a flow was added to the frontier with nothing to seed its program sequence — the session installs that "
           "hook when it opens, so this flow was created outside a session");
    /* The seed parks the new flow's program requests, and a park reads the document and records an endpoint
       against flow_running(); with another member switched in, both would be that member's (its DOM delta, its
       account and its candidate suppression). A from-baseline flow's view is the baseline. */
    DCHECK(g_running == NULL,
           "a flow was seeded while another member holds the thread — switch that member out first, since the "
           "seed's parks read its document and credit its account");
    g_seed_hook(f);
}

Flow *flow_add(JSContext *ctx, JSValueConst fn, WorldId parent) {
    Flow *f = flow_add_unseeded(ctx, fn, parent);
    flow_seed(f);
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
           "a routed delivery was queued with no sender origin — only the trusted zone stamps one, and every "
           "`event.origin` check reads it");
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
           "a delivery-world commitment carried something other than received (1) or foreclosed (0)");
    /* Through flow.h's membership test rather than a list of names here: a second copy of the enum's set is the copy
       that drifts when a member is added. */
    DCHECK(flow_commit_arm_is_member(arm),
           "a delivery-world commitment named a minting mechanism outside flow.h's FlowCommitArm — a producer states "
           "one of its members, so this is a producer never given a member or a park document this engine did not "
           "write");
    /* And a foreclosed row's other side is its parent, by what the row means: "this flow is the arm minted where its
       parent took `vector`", so a foreclosed row naming no other side contradicts itself. Asked here rather than at
       the one live producer because the cold tier replays rows off a document, which can hand over a pair no live
       code makes. */
    DCHECK(taken != 0 || arm != FLOW_COMMIT_ARM_NONE,
           "a receiving timeline foreclosed a sender subtree while naming no flow on the other side of the branch — "
           "a foreclosed row says this flow is the arm minted where its parent took that world, so the other side "
           "cannot be absent");
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
                        "a receiving timeline committed to two sending worlds that contradict, pushed from %s:%d "
                        "(arriving <%s>, held <%s> with FlowCommitArm %d, -1 meaning no integer was stored) — the "
                        "received rows must be pairwise consistent, so the repair is the pushing producer's and this "
                        "check is never widened to admit the pair",
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
    DCHECK(!g_job_external, "a second routed record was turned into local work while the first was in flight — "
                            "deliveries are made from flow_step with no frame, so the brackets cannot nest");
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
           "a job-kind split was asked for with nowhere to put one of its two halves");
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
           "a queued job was asked for by the never-issued handle — records that never went through the runtime's "
           "enqueue carry exactly that handle");
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
                   "a tracker's removal by handle named the task a routed cross-document delivery became — "
                   "deliveries carry JS_TASK_HANDLE_NONE so no handle can name them, so two records share one "
                   "handle");
            found++;
            JS_FreeValue(ctx, e);
            continue;
        }
        JS_SetPropertyUint32(ctx, f->jobs, (uint32_t)keep++, e);
    }
    JS_SetPropertyStr(ctx, f->jobs, "length", JS_NewInt32(ctx, keep));
    cow_engine_write_end();
    DCHECK(found <= 1,
           "one handle named two jobs on one flow's queue — handles come from one monotone runtime counter and a "
           "fork names each record once per arm, so two entries share one handle");
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
    DCHECK(f != NULL, "the fitness term was asked of no flow");
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
           "a branch bucket's contribution to the order was asked of no bucket — flow_new opens one for every node "
           "and the fork moves membership rather than clearing it");
    DCHECK(br->sub_born >= 1,
           "a branch bucket reports a lifetime mint count below one — `sub_born` is written to 1 at flow_new and "
           "only raised after");
    DCHECK(br->sub_offset == 1.0 / (double)br->sub_born,
           "a branch bucket's stored offset is not `1.0 / sub_born` — flow_new and flow_fork_inherit's join write "
           "the two together, so a third writer of `sub_born` left the offset standing, or this host evaluates "
           "doubles wider than binary64");
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
           "membership rather than clearing it");
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
           "a member's branch bucket was asked for with no bucket — flow_new opens one for every node and the fork "
           "moves membership rather than clearing it");
    DCHECK(f->acct->branch->sub_born >= 1,
           "a member's own branch bucket reports a lifetime mint count below one while the member stands in it — "
           "`sub_born` is written to 1 at flow_new and only raised after");
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
           "the member half of the WFQ's weight is not its per-member key plus its bucket term — the two spellings "
           "associate identically, so a summand reached one and not the other, or this host evaluates doubles wider "
           "than binary64");
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
           "the member half of the WFQ's weight is not its per-member key plus `1/sub_born` rebuilt from "
           "flow_branch_born — flow_new and the fork's join write the offset with `sub_born`, so the two came apart "
           "or this host evaluates doubles wider than binary64");
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
           "a term of the WFQ's order other than the reward exceeds FLOW_NONREWARD_MAX — every non-reward term must "
           "span at most a finding; bound the term in flow_queue_nonreward or flow_distance, or state its range in "
           "FLOW_NONREWARD_MAX");
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
           "the member half of the WFQ's weight differs from flow_nonreward with the family's aging at zero and no "
           "carry — there the two are the same expression on the same operands, so a summand reached one and not the "
           "other");
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
           "a flow reported host-owed again with no host event in between — a marked flow is out of the pick, so its "
           "mark was cleared by something that cannot have answered it");
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
       host reaches this between slices (the provider's own edges), so the yield request is
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
    DCHECK(f != NULL, "an image was asked of no flow — paint_mark_standing_members walks the frontier's own members "
                      "and flow_new marks the flow it just allocated");
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
           "an image ask was discharged on a member that owed none — every discharge tests the mark it clears, so "
           "this route never took the ask");
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
           "an order-scan count was asked for an entry not in solver/flow.h's FLOW_SCANS list");
    return g_scan_runs[s];
}
int64_t flow_scan_weights(FlowScan s) {
    DCHECK((unsigned)s < (unsigned)FLOW_SCAN_N,
           "an order-scan weight count was asked for an entry not in solver/flow.h's FLOW_SCANS list");
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
          "engine: the order was asked through a scan entry not in solver/flow.h's list — the value is written only "
          "from that enum at a flow_pick call site, so this is a cast or an uninitialised read");
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
    DCHECK(!(seed && worst), "the eviction tail was asked with an incumbent to defend — a seed names who keeps the "
                             "thread, which is not a question the pager asks");
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
                    "a non-running member changed its own half of the WFQ's weight at generation %u with no "
                    "generation bump — only the family's common half and the carry bit may move between generations, "
                    "so a writer of the optimism, distance, branch bucket or own silence skipped its bump, or a "
                    "weighing landed between flow_fork_inherit's `sub_born++` and its bump (member half %.17g -> "
                    "%.17g, phase %lld -> %lld, index key %.17g -> %.17g)",
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
                    "the index surrogate stands further from the comparator than flow_index_margin's derived bound "
                    "allows — a summand was added to flow_weight, a term's range left FLOW_NONREWARD_MAX, or this "
                    "host rounds at more returns than clause (b) allows. surrogate=%.17g weight=%.17g "
                    "|difference|=%.17g margin=%.17g, over val=%.17g notch=%lld optimism=%.17g distance=%.17g "
                    "branch=%.17g",
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
                "the surrogate's extremum is not extremal over a population containing the comparator's pick — both "
                "folds admit `best` on the same gate and direction bit, so the surrogate's fold dropped it. "
                "sur_w=%.17g, surrogate of the comparator's pick %.17g, over %d member(s), worst=%d",
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
                    "the candidate set carrying the derived margin did not return the comparator's member — either "
                    "the band's edge is wrong (`2*sur_mmax` over the whole folded population, seed included) or the "
                    "band walk's order, strictness or seed fold differs from the scan's. The band held %ld of %ld "
                    "member(s) tested, edge=%.17g from sur_w=%.17g and 2*margin=%.17g; the band returned slot %d "
                    "weighing %.17g and the scan slot %d weighing %.17g",
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
                    "the surrogate's tie and strict counts do not sum to the scans in which it named another member "
                    "— the total and the arms are raised over one condition at two sites, so the two sites came "
                    "apart. tie=%ld strict=%ld differed=%ld",
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
           "the WFQ re-picked a flow whose own and family silence exceed what its reward is worth while a never-run "
           "flow waited — the aging term is no longer commensurate with the reward (flow_silence_us_to_sink derives "
           "the bound)");
    /* And the other half of the pair: "a UCB optimism bonus ∝ 1/(visits+1) so a NEVER-RUN FLOW IS NEVER STARVED".
       The bound above says a silent monopolizer sinks, not that the waiting flow is lifted, and the two can break
       separately. Also an identity derived from flow_weight: an untouched member weighs `val + 1.0` or more, and
       `best` is the maximum over the scan that set `unrun`. It is false once the optimism bonus is keyed on a
       quantity a never-run flow does not stand at zero of (thread time, say, since an arrival once stood at the
       running flow's service), given a range other than 1.0, or made to depend on anything the aging already
       prices; each lets "never starved" stop meaning anything while every other assertion passes. */
    DCHECK(worst || !best || !unrun || flow_weight(best) >= 1.0,
           "the WFQ picked a flow worth less than an untouched member of its frontier — a never-run flow carries the "
           "full optimism bonus and no aging, so the pick's maximum must be at least 1.0");
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
    /* Starvation, counted at the line that decides; the census's gauges say a tied tail exists, not that a pick
       passed over it. It counts the dispatch pick returning a member dispatched before while a never-dispatched one
       stood at the same weight, only where the pick displaces the incumbent (`best != seed`): engine.c credits a
       dispatch inside `if (best != cur)`, so this and `picksLifetime` count one event and `starvedPicks <=
       picksLifetime` holds (result.c asserts it). Counted, not asserted: one such pick is legitimate (ties go to the
       incumbent), and starvation is a rate. It does not separate a framed flow finishing its program while an arm
       born at its weight stands tied from a pass over a member with nothing in front of it; `g_starved_picks_idle`,
       the subset whose member was between units (flow_between_units) with no parked continuation, does. Since
       `best != seed`, every member counted has its park queue on `Flow::parked`, which flow_holds_park reads
       exactly. The complement, the incumbent keeping the thread level with a never-run member, is the plateau rows
       below. Exact `==`: two members at one weight read one value twice. Lifetime counters off the flows, reset by
       nothing; both raised under one condition, so `idle <= starved` by construction. */
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
        /* And the mint pair, over the same population and inside the same guard, because it is the order's range,
           not a receipt: `sub_born` is what flow_branch_bonus divides by, so these extrema are the branch term's
           range (wfq_accounted_spread reads them so). A weight is read only for a standing member, and a member
           stands only in a bucket with a live count, so extrema over an empty bucket describe a range nobody spans;
           folded by separate statements, the two pairs are one population only through this guard. flow.h declares
           `br_born_max` as "the most members ever MINTED into one live bucket", paired with `br_live_max`. The
           `br_us_*` pair keeps every taken bucket, because receipt is a different question from membership. */
        if (out->br_born_min == 0 || br->sub_born < out->br_born_min) out->br_born_min = br->sub_born;
        if (br->sub_born > out->br_born_max) {
            out->br_born_max = br->sub_born;
            /* And the bucket itself, retained rather than copied, for the crowd pointer's reason:
               `br_minter_live + br_minter_gone == br_born_max` then compares a second reading of the bucket's pair
               (dereferenced once, after the walk) against this running maximum. The mint count is the selector, so
               this names the arm that has taken the most arms and the crowd the arm that holds the most; they are
               one bucket only while nothing has been shed. Strict `>`, as the crowd's, so the maximum and the
               pointer stay at the first of two equal buckets. */
            g_wfq_census_minter = br;
        }
    } else {
        /* The empty half, the ordinary state of a frontier whose boot flow has finished: the family-root door takes
           such a bucket to keep boot's burn, real receipt, inside the identity, which is why `br_us_sum` is the wrong
           denominator for a live arm's share.
           Which buckets may be here at all, asserted: the member door passes `f->acct->branch` for a live `f` whose
           own mint has not been shed, so it cannot reach this arm. Everything here came through the family-root
           door, where `Flow.family` names a root (`up == NULL`, asserted at the dispatch) whose bucket stands at the
           `sub_born == 1` flow_new wrote, so `live == 0` is its own flow having departed; acct_depart clears `owner`
           before raising `sub_gone`, so both halves are readable at one instant. Unlike `br_live_sum == members`,
           which compares two sums any compensating pair satisfies, this fires on a live member standing in a bucket
           whose count says nobody does. */
        DCHECK(br->up == NULL && br->owner == NULL,
               "the census took an empty branch bucket that is not a departed family root — the member door reaches "
               "only buckets with a live member, so a join raised `sub_born` on the wrong bucket, a departure raised "
               "`sub_gone` twice, or a root was forked into");
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
    /* The best weight any ready job holder offers, held as a separate maximum because `job_w_gap` is a difference
       against a top known only when flow_best runs below, and "no ready holder" must differ from "a ready holder
       at weight zero". */
    double job_w_max = 0.0;
    int have_job_holder = 0;
    /* And the same pair for the reply backlog, for the same reason (`deliv_w_gap`). */
    double deliv_w_max = 0.0;
    int have_deliv_holder = 0;
    /* And the member itself, because `deliv_w_gap`'s reading needs a term and a weight is a sum (flow.h's
       `deliv_w_gap_vis`). A pointer rather than a copied count, written by the same `if` as the maximum. */
    const Flow *deliv_holder = NULL;
    /* The best weight any never-dispatched member offers, held as the job holder's is (`never_picked_gap`). */
    double never_w = 0.0;
    int have_never = 0;
    /* And how many never-dispatched members stand at that maximum: collected here and attributed to the front once
       flow_best runs below. It costs no weighing; the walk has this member's weight. */
    long never_at_w = 0;
    /* And the deepest row of the document's program table any member stands at, with the best weight there and how
       many stand with it, collected here for `cur_deep_w_gap` and attributed to the front below. The `have_` flag
       keeps "no member standing" apart from "a member at row 0", where every member of a fresh frontier is. */
    int cur_deep = 0;
    long cur_deep_live = 0;
    double cur_deep_w = 0.0;
    int have_cur_deep = 0;

    /* The weighings flow_best performs below, read before it runs: FLOW_SCAN_OTHER is shared (the host's best-weight
       read and the pager's tail run under it), so bracketing is the only way to know what this call spent. The
       identity at the end of this function is stated against it. */
    long other_before;

    DCHECK(out != NULL, "the WFQ was asked to report its ordering into nothing");
    /* This walk, counted (flow.h's FLOW_SCANS), at the top for flow_pick's reason: every exit is one walk. */
    g_scan_runs[FLOW_SCAN_CENSUS]++;
    other_before = g_scan_weights[FLOW_SCAN_OTHER];
    out->members = g_flows_n;
    out->val_min = out->val_max = out->val_top = 0.0;
    out->val_zero = out->self_emit = out->unrun = 0;
    out->val_arrived = out->val_unplaced = 0;
    /* The clock itself, not a reading of this walk: the frontier's virtual time at the census, where every unplaced
       account stands, assigned unconditionally as `nonreward_max` and `picks_lifetime` are. Without it a reader
       cannot tell a `valMin` tracking the clock from one left behind by it. */
    out->vt = frontier_vt();
    out->never_picked = 0;
    out->never_picked_gap = 0.0;
    out->never_picked_at_top = 0;
    out->picks_live = out->picks_max = 0;
    /* The lifetime half, not a reading of this walk: dispatches this instance has ever made, unconditional, and the
       only one of the three pick rows a reader may difference across two censuses. */
    out->picks_lifetime = g_picks_total;
    /* And the share of it that left: the half the two gauges cannot see, so publishing it makes their partition
       checkable from outside the process, where the DCHECK below is compiled out. */
    out->picks_departed = g_picks_departed;
    /* And its unframed subset, for the same reason: a count of dispatches, the same on an empty scan as a full one,
       published so the containment below is checkable on the document. */
    out->unframed_picks_lifetime = g_unframed_picks_total;
    /* And its ready subset, likewise, so both containments are checkable on the document. */
    out->ready_picks_lifetime = g_ready_picks_total;
    out->svc_max = out->svc_min = out->svc_fam_max = out->svc_fam_min = 0;
    /* And the two rows that price asking the notch, cleared beside it because they are read over the same walk off
       the same quantity (flow_silence_phase). The residue map is cleared here, not at its declaration, because a
       byte surviving from the previous sample would count a phase no member stands at. */
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
    /* And the retained buckets, cleared before the walk as well as after they are read: a pointer from the previous
       census may name a node this one does not reach and a later departure has freed. */
    g_wfq_census_crowd = NULL;
    g_wfq_census_minter = NULL;
    out->br_depth_max = 0;
    out->br_fan_max = out->br_fan_sum = 0;
    out->br_fan_depth = 0;
    /* The two instance totals, not readings of this walk, assigned unconditionally: the other two terms of the burn
       identity asserted at the end of this function, published so it is checkable from outside the process. */
    out->br_retired_us = g_branch_retired_us;
    out->charged_us = g_charged_us;
    /* A fresh mark for this scan, past zero, so a node minted since the last census (calloc'd to 0) reads as
       unmarked. The bucket walk shares this generation value with a mark of its own (`branch_gen`), because a root
       is both a family and a branch and one field cannot record being counted once as each. */
    if (++g_wfq_census_gen == 0) g_wfq_census_gen = 1;
    out->vis_min = out->vis_max = 0;
    out->cand_members = out->cand_unrun = out->cand_dec_max = 0;
    out->cand_svc_max = 0;
    out->dec_max = 0;
    out->dist_max = 0.0;
    out->w_top = out->w_min = out->cand_w_max = 0.0;
    out->top_svc = out->top_svc_fam = 0;
    out->top_forgiven = 0;
    /* The bound the gap rows are read against, the macro flow_nonreward asserts: a property of the formula, the same
       on an empty scan as a full one, which is why it can be the denominator of a reading about members. */
    out->nonreward_max = FLOW_NONREWARD_MAX;
    out->jobs_ready = out->jobs_framed = out->jobs_owed = out->vis_zero = 0;
    /* And the two halves of the ready row, zeroed on their own line so the partition is one statement; a half left
       un-zeroed would accumulate across censuses and fire the sum assert at the end of this scan. */
    out->jobs_ready_task = out->jobs_ready_micro = 0;
    out->mem_unframed = 0;
    out->job_w_gap = 0.0;
    out->deliv_ready = out->deliv_framed = out->deliv_owed = 0;
    out->deliv_w_gap = 0.0;
    /* And the two ends of that gap, zeroed beside it because they are only written beside it: a frontier with no
       ready holder states no gap and no counts. */
    out->deliv_w_gap_vis = out->w_top_vis = 0;
    /* And the same for the document-progress gap and its population, written in one branch below. */
    out->cur_deep = 0; out->cur_deep_live = 0; out->cur_deep_w_gap = 0.0;
    for (i = 0; i < g_flows_n; i++) {
        const Flow *f = g_flows[i];
        int64_t s = flow_service_notch(f);
        /* And the other side of the away population's identity, computed from the accessor while the maintained
           count is folded below from the family door: every member whose flow_own_silence is non-zero stands away
           from its family's epoch base. A different route from `epoch_away_live` (four incremental statements at
           four sites), so the assertion at the end is a check and a fifth transition site fires. */
        if (flow_own_silence(f) > 0) out->epoch_away_walk++;
        /* The sum the pick uses, not only its terms (flow.h says what went wrong without it). This scan decides
           nothing. */
        double w = flow_weight(f);
        /* And priced where it is taken, as flow_pick prices its own: weight evaluations, so the rows are one
           quantity. This walk skips nothing, so its count is the frontier's size per sample, which makes the
           identity below an assertion. */
        g_scan_weights[FLOW_SCAN_CENSUS]++;
        /* And where this member stands in the document's program table, kept for the deepest row anybody is at
           (flow.h). Rewritten when a deeper member is met and accumulated on an equal one, because the population
           is the members at the maximum. It costs no weighing. `Flow.script_i`, the field cold.c's
           `program_cursors` histogram buckets on, so this maximum can be checked against that histogram's top
           non-empty index. */
        if (!have_cur_deep || f->script_i > cur_deep) {
            have_cur_deep = 1; cur_deep = f->script_i; cur_deep_live = 1; cur_deep_w = w;
        } else if (f->script_i == cur_deep) {
            cur_deep_live++;
            if (w > cur_deep_w) cur_deep_w = w;
        }
        /* How many decisions this flow stands on, read from where its decision state lives: a parked flow's blob,
           or decide.c's live globals for the flow switched in (cold.c's census makes the same split). Asked of every
           member, because the candidates' figure means nothing without it. */
        long dec = 0;
        /* And the position along it, a different number and the one `cand_dec_max` reports. `dec` is the chain's
           length (decide.c: "a caller that wants a DISTANCE TRAVELLED must not take this number: it is the
           denominator"), and an @S candidate is seeded with the whole detecting chain under a cursor of zero, so for
           a candidate only the cursor moves. Both come from where the state lives: `Flow.dec_blob` is NULL exactly
           while a flow runs (engine.c frees it at the switch-in), so a blob-only reading would report zero for the
           incumbent, under-reading whoever holds the thread. The running flow's cursor is decide_cursor(). */
        long cur = 0;

        if (f->dec_blob) { decide_blob_stats(f->dec_blob, &dec, NULL); cur = decide_blob_cursor(f->dec_blob); }
        else if (f == g_running) { decide_live_stats(&dec, NULL); cur = decide_cursor(); }
        if (dec > out->dec_max) out->dec_max = dec;
        /* A position cannot lie outside the path it is on, asserted where both are in one hand. Each source checks
           its own half (decide_blob_cursor against its blob, decide_live_stats and decide_cursor against the live
           vector), and neither sees the pair this loop composes from two calls. */
        DCHECK(cur >= 0 && cur <= dec,
               "the WFQ census composed a replay position outside the path it is a position on — the length and the "
               "cursor must come from the same member's decision state");

        /* The reward rows read the term the order is made of, the family's. Reached per member, so a family with N
           arms contributes its reward N times to these extrema, which is right for a spread (the range the pick sees
           across the members it ranks) and is why `families` is reported beside them: on a one-family frontier the
           spread is zero by structure, and a non-zero one is about several accounts.
           No member may hold more than its account: `Flow.val` is what this member emitted, every point of it
           credited to its family in the same call, so a member above its family is one whose two writers came apart,
           visible otherwise only as a `self_emit` row counting emissions the order never saw. */
        DCHECK(f->val <= acct_family_val(f) ||
               f->family == NULL,   /* departed members are not walked, but the read is stated as the pair */
               "a member of the frontier has emitted more than the fork family it is credited to holds — the "
               "two writers of one emission have come apart, so the order is ranking an account that is "
               "missing findings this member's own ledger already counted");
        if (i == 0 || acct_family_val(f) < out->val_min) out->val_min = acct_family_val(f);
        if (i == 0 || acct_family_val(f) > out->val_max) out->val_max = acct_family_val(f);
        if (acct_family_val(f) == 0.0) out->val_zero++;
        /* And accounts that have earned nothing, whatever coordinate they were placed at: an arrival stands at the
           frontier's virtual time (flow_arrive_at_virtual_time), so an @S candidate session can stand at the
           leader's reward having emitted nothing, outside the zero row and exactly the population a pinned
           `val_min` is about. One field read, because FlowAcct's reward is two quantities. */
        if (f->family && f->family->earned == 0.0) out->val_arrived++;
        /* And the subset whose coordinate is still a reading rather than a tag, which separates a candidate the
           order has never reached from one it served and passed: `valArrived - valUnplaced` is the population that
           held the thread and emitted nothing, the members a search should be mutated toward. Read off the account,
           since placement is a fact every arm reads through one pointer. */
        if (f->family && !f->family->placed) out->val_unplaced++;
        /* And what this member itself put there, the question the account cannot answer: nothing is inherited, so a
           member's ledger is what it emitted since birth. */
        if (f->val > 0.0) out->self_emit++;
        /* How many members stand at zero of the aging's own half, read through flow_own_silence, the quantity the
           order is made of, rather than `Flow.cpu`, which is that quantity only while its window mark is current. */
        if (flow_own_silence(f) == 0) out->unrun++;
        /* And the population the row above cannot name: zero own silence is also what an emission by any arm of the
           family produces. A member never handed the thread is the population starvation is about, and `picks` is
           the one field nothing resets. The best weight among them goes to the gap below: how far the most-favoured
           starved member stands behind the pick is the reading, which the count cannot make (flow.h). */
        if (f->picks == 0) {
            out->never_picked++;
            /* The maximum and its width in one pass, without a second weighing: the width resets with the maximum,
               and every later member at exactly that weight joins it. Exact `==` with no epsilon, this file's idiom
               (two members at one weight read one value twice), so the plateau is one the order has. */
            if (!have_never || w > never_w) { never_w = w; have_never = 1; never_at_w = 1; }
            else if (w == never_w) never_at_w++;
        }
        /* And how the dispatches that did happen were distributed over the members that got them, which
           `never_picked` cannot ask. `never_picked` climbing beside `members` is one answer covering three states.
           A frontier of M members, P of them ever chosen, and T dispatches spent is:
             T / P near 1   the thread reached a fresh member almost every time and the frontier grows faster
                            than one thread serves it: throughput, which no weight change reaches.
             T / P large,
             picks_max
             near T / P     a reachable cohort is swept repeatedly while the tail waits: the order returns members
                            it has served ahead of members it has not, which a weight term must answer for.
             picks_max
             near T         one member holds the thread and the switches are it and one rival trading: a
                            monopolizer the aging term fails to sink, a different repair.
           `picks_live` is the numerator (T over standing members) and `picks_max` tells the second state from the
           third; `members - never_picked` is P. A gauge; `g_picks_total` is the lifetime half, held off the flows,
           and flow.h's row states the identity that ties them. */
        out->picks_live += f->picks;
        if (f->picks > out->picks_max) out->picks_max = f->picks;
        /* The optimism term's coordinate, completed units (flow.h). `vis_max == 0` on a frontier of thousands says no
           member has reached the end of a program, so no queued job can have run, whatever the switch and fork
           counts say. */
        if (i == 0 || f->visits < out->vis_min) out->vis_min = f->visits;
        if (f->visits > out->vis_max) out->vis_max = f->visits;
        /* And how many members stand at zero of it, which `vis_min` cannot say (flow.h). */
        if (f->visits == 0) out->vis_zero++;
        /* And how many hold no frame, taken here so it and `members` are one sample from one walk; it separates
           `jobs_ready`'s two zeroes. It is not the job arm's predicate: the split asks flow_stack_empty, which has a
           second half (a row at the cursor marked DYN_POS_IMMEDIATE, HTML §8.1.4.4 "Calling scripts"' clean up after
           running script step 3), while this row counts `frame == NULL`. The two are a containment, asserted per
           member: the tail's asserts are over totals (`jobs_ready == 0 || mem_unframed > 0`) and cannot see a
           re-spelling of `mem_unframed`, while here the assert and the count read one expression, so a predicate
           that stops containing flow_stack_empty's population fires at the member that breaks it. One-sided by
           design: a member holding no frame whose cursor names a DYN_POS_IMMEDIATE row is HTML §4.12.1.1
           "Processing model"'s "Otherwise, immediately execute the script element el, even if other scripts are
           already executing", inside `mem_unframed` and refused by flow_stack_empty, so `==` would fire on every
           document that injects a script from a script. A design invariant whose operands live in two functions far
           apart and are edited independently; its absence would show as `jobsReady` or `delivReady` above zero
           beside a `memUnframed` that no ready member is inside, which both tail asserts pass. */
        {
            const int unframed = (f->frame == NULL);
            DCHECK(!flow_stack_empty(f) || unframed,
                   "a member whose JavaScript execution context stack is empty was not counted as unframed — "
                   "flow_stack_empty refuses a live frame first, so this row's predicate must contain its "
                   "population");
            if (unframed) out->mem_unframed++;
        }
        /* The job backlog split by what it waits on, in the order the engine asks: flow_pick refuses a host-owed
           member, so that comes first; among the rest, HTML §8.1.4.4 "Calling scripts"' clean up after running
           script step 3 ("If the JavaScript execution context stack is now empty, perform a microtask checkpoint")
           is what every job arm of flow_step is under; whatever is left waits on rank, the one class the order can
           move. if/else-if/else, so the classes are disjoint and exhaustive. Asked through the arm's own guard:
           flow_run_one_job's two call sites are guarded on flow_checkpoint_due (`flow_job_microtask &&
           flow_stack_empty`) and the task arm, and flow_stack_empty is that sentence (a live frame or a
           DYN_POS_IMMEDIATE row at the cursor), so an edit to the checkpoint's precondition reaches this census.
           `job_w_gap` is `w_top` minus the best weight a ready holder offers, both through flow_weight from one
           walk, so zero says the front of the order is one of these members. Three readings then take opposite
           work: the dispatch does not take them; it takes them and flow_step's ladder declines the job at an arm
           above the one that would run it; or `w_top` and `job_w_max` are not what the dispatch compares.
           `ready_picks_lifetime`, beside `unframed_picks_lifetime`, separates the first two (zero with the superset
           non-zero is the first; above zero with `_jobsRun` flat the second; flow.h's legend at
           `unframed_picks_lifetime`).
           Named residual. Not covered: this row is exact for a microtask and narrower than the question for a task.
           A member satisfying flow_stack_empty that holds a task job while its cursor names a runnable row is
           reached only in arrival order (flow_task_precedes, engine.c's task ladder), so this row calls the job
           rank-ready while the ladder, not the rank, holds it; `jobs_ready_task` sizes that population. Next diff
           builds: the ladder's arm order settled at flow_step, after which reachability is answered there rather
           than inferred from this census. Absence shows as: a census with `jobsReady` above zero and `jobWGap` at
           zero on a run whose lifetime `_jobsRun` never leaves zero, the ordinary state on real documents. */
        {
            int jn = flow_job_pending(f);
            if (jn > 0) {
                if (flow_host_owed(f))         out->jobs_owed   += jn;
                else if (!flow_stack_empty(f)) out->jobs_framed += jn;
                else {
                    int jt, jm;

                    out->jobs_ready += jn;
                    /* And split again on the axis that decides which arm of flow_step can take the job (flow.h): a
                       microtask's arm (the checkpoint) stands above the program sequence and a task's below it, so
                       `jobs_ready` calls rank-ready two populations of which one also waits on the ladder. Taken
                       inside the arm that admitted this member, so the queue walk is paid only for the population
                       `mem_unframed` bounds. Both halves come from flow_job_kinds, neither is `jn` minus the
                       other, so the sum asserted at the end of this scan can fail. */
                    flow_job_kinds(f, &jt, &jm);
                    out->jobs_ready_task  += jt;
                    out->jobs_ready_micro += jm;
                    /* The best of them, against which `job_w_gap` is taken: how far the backlog's best claim stands
                       from the front of the queue. */
                    if (!have_job_holder || w > job_w_max) { job_w_max = w; have_job_holder = 1; }
                }
            }
        }
        /* And the same three questions of the reply backlog (flow.h), asked through the delivery arm's own guard
           (flow_stack_empty), the predicate engine.c's arm is written against, as the job rows are. Same
           if/else-if/else shape. `pending_ready` rather than `pending_deliverable_count`: cold_census walks the
           register once per report for the debt, and this needs only whether this member holds one. */
        if (pending_ready(f->pending)) {
            if (flow_host_owed(f))        out->deliv_owed++;
            else if (!flow_stack_empty(f)) out->deliv_framed++;
            else {
                out->deliv_ready++;
                /* The best of them, against which `deliv_w_gap` is taken, a maximum for the job holder's reason. */
                if (!have_deliv_holder || w > deliv_w_max) {
                    deliv_w_max = w; have_deliv_holder = 1; deliv_holder = f;
                }
            }
        }
        if (s > out->svc_max) out->svc_max = s;
        /* And the sub-quantum remainder `s` threw away, which decides whether asking the order has to walk at all:
           `s` is `own / S` and this `own % S`, the member's phase against the family's and the one per-member
           quantity in flow_weight that moves between two frontier generations (flow_silence_phase). Distinct
           values rather than a spread, because an index needs to know how many groups the frontier partitions into:
           `sil_phases: 1` is the strongest answer the arithmetic permits (every member crosses the carry boundary
           at once, so the bit is a common offset and a single cached maximum is exact), and a larger reading is the
           number of ranges a sweep moves through. It is not a typical engine reading; solver/flow.h carries the
           measurement, its corpus and the retirement condition. */
        {
            int64_t ph = flow_silence_phase(f);
            if (!g_phase_seen[(size_t)ph]) { g_phase_seen[(size_t)ph] = 1; out->sil_phases++; }
            out->sil_carry += flow_silence_carry(f);
        }
        /* And the family this member belongs to, in the same notch, the one the aging reads, so `svc_fam_max`
           against `svc_max` is the order's denominator against one member's share. Read per member, since a family
           has no member of its own to ask; the maximum reaches every family as often as it has arms standing. */
        {
            int64_t fam = flow_family_notch(f);
            /* And which family this is, which the extrema cannot supply: `svc_fam_min == svc_fam_max` holds by
               structure on a one-family frontier (every member reads one `fam_us` through one pointer), where the
               family half can never order, and only contingently on several families, where it is ordering and
               momentarily level. Families can also be born level: a from-baseline flow founds its own family at zero
               silence and diverges once charged (flow_age_running bills the running family alone) or forgiven
               (flow_credit_emit's window bump). Counted by identity, not value. */
            DCHECK(f->family != NULL,
                   "a live member of the frontier belongs to no fork family — the aging term reads its "
                   "family's service, so this flow is charged zero for every microsecond its chain burns and "
                   "rides above every member that pays");
            if (f->family && f->family->census_gen != g_wfq_census_gen) {
                f->family->census_gen = g_wfq_census_gen;
                out->families++;
                /* And this family's maintained away count, folded at the one door that visits each family exactly
                   once: `census_gen` makes it a sum over distinct families. */
                out->epoch_away_live += f->family->away_n;
                /* And the root's own branch bucket, opened here where the root is in hand and guarded. A root's
                   bucket holds exactly one member, the root flow (every arm it forks opens a bucket of its own), so
                   once the root flow departs its node is held alive by its children's `up` references while nothing
                   names it through `branch`, and its burn (boot's, on a real page) would be in neither the live sum
                   nor the retired term: the one way the burn identity could fail on a healthy frontier. Once per
                   distinct family per census; the node's own mark makes it idempotent against the member door below,
                   which reaches the same node while the root flow stands. */
                DCHECK(f->family->branch == f->family,
                       "a family root is not its own branch bucket — flow_new points a node's `branch` at itself and "
                       "only a fork moves it, and a root is never a forked arm");
                branch_take(out, f->family);
            }
            if (fam > out->svc_fam_max) out->svc_fam_max = fam;
            /* And its floor, which says whether the term orders or offsets (flow.h): the pair states how much of the
               family half any comparison can see, exactly zero on a one-family frontier. */
            if (i == 0 || fam < out->svc_fam_min) out->svc_fam_min = fam;
        }
        /* And the branch bucket this member's subtree is counted in, the scope between the member's silence
           (`svc_max`/`svc_min`) and the family root's (`svc_fam_*`, a common offset when everything descends from
           boot): how much of the thread each side of a branch received. Counted by identity with the same mark, each
           bucket opened once per census, so the sum is a partition of the frontier. `br_live_max` against `members`
           is how concentrated the frontier is in one side of one top-level branch; `br_us_max` against `charged_us`
           how concentrated the thread is; `br_born_max` against `br_live_max` separates a bucket that mints without
           bound from one that holds a lot now, which take opposite diffs. Each maximum belongs to whichever bucket
           owns it (the burn maximum is often a departed family root with no live member), so one bucket's
           membership, mint and receipt are read from the crowd rows after this walk. */
        {
            FlowAcct *br;
            DCHECK(f->acct != NULL,
                   "a live member of the frontier has no fork-tree node — the node is minted at flow_new and "
                   "released only by the departure that removes the flow");
            br = f->acct->branch;
            DCHECK(br != NULL,
                   "a live member's fork-tree node names no branch bucket — flow_new points every node at "
                   "one and flow_fork_inherit only ever re-points it at another, so this member's thread "
                   "time is being charged to no side of any branch");
            /* Through two doors, which is the burn identity's soundness: every live bucket must be reached or its
               lifetime burn is in neither the live sum nor the retired term. A non-root bucket is reached by its
               subtree (a node lives only while it has a live-owner descendant, and every descendant names it). A
               family root's is reached through `Flow.family` (it points at the root, whose `branch` is itself),
               opened in the family-root block above, which already tests `f->family` before dereferencing and fires
               once per distinct family; opening it here would dereference in release a pointer only a DCHECK
               guards. The node's own generation stops the root being counted twice while its flow stands. */
            branch_take(out, br);
            /* And how deep the deepest live member sits in the fork tree, taken where the node is in hand: whether
               the tree this engine builds is a star or a chain, a shape every other row is blind to. */
            if (f->acct->depth > out->br_depth_max) out->br_depth_max = f->acct->depth;
            /* And which fork minted this member, which the bucket sums away: a bucket is a top-level arm, so the
               rows above cannot say whether an arm minted the crowd at its first branch or four levels down. Keyed on
               `up`, the true fork parent: its writers are flow_fork_inherit at the fork and acct_compress_dead, which
               runs only from acct_depart on the departing flow's own node, so a live member's edge is written once.
               The parent may be dead and retained by this member, the informative case (a walker that forked N arms
               and finished). Non-root parents only: a fork off a root opens its own bucket (flow_fork_inherit's `br`
               expression), so the rows above separate it; the gate uses the same predicate. A gauge over standing
               members that may fall between samples; `br_born_max` is the lifetime question. */
            {
                FlowAcct *up = f->acct->up;
                /* The bucket a child inherits, read back where the children are counted: the write side is
                   flow_fork_inherit's `br = parent->acct->up ? parent->acct->branch : sib->acct`, asked here at a
                   second instant. A compression that moved a live member's edge, or a `branch` re-pointed by
                   something other than the fork, separates them. This makes the fan identity at the end hold by
                   construction: a fan cannot outnumber the bucket holding it. The root arm is asserted where the
                   fork decides it, and root-parented members are excluded from the fan by the gate below. */
                DCHECK(up == NULL || up->up == NULL || f->acct->branch == up->branch,
                       "a live member's branch bucket is not its fork parent's — a fork off a non-root joins the "
                       "parent's bucket and nothing else re-points `branch`");
                if (up && up->up) {
                    if (up->fan_gen != g_wfq_census_gen) { up->fan_gen = g_wfq_census_gen; up->fan_n = 0; }
                    up->fan_n++;
                    out->br_fan_sum++;
                    /* The depth is taken with the maximum, since it is a fact about the node holding it; `>` keeps
                       the shallowest of two tied forks, the one nearer the bucket. */
                    if (up->fan_n > out->br_fan_max) {
                        out->br_fan_max = up->fan_n;
                        out->br_fan_depth = up->depth;
                    }
                }
            }
        }
        /* The floor, over every member (flow.h): a frontier holding one never-run flow reads 0, and that is the
           answer, the case the aging term was priced for. `svc_max` read the other way, same scan and unit. */
        if (i == 0 || s < out->svc_min) out->svc_min = s;
        if (i == 0 || w < out->w_min) out->w_min = w;
        /* The fitness term's range: its floor is 0 by the type (flow.h), so the maximum is the range. Over every
           member, because the bound below is over every member's weight. */
        { double d = flow_distance(f); if (d > out->dist_max) out->dist_max = d; }
        /* And the same questions of the candidates alone (flow.h). `cand_src` is what a candidate session is, and
           engine.c copies it to a sibling, so this counts the search's whole live population. */
        if (f->cand_src) {
            if (!out->cand_members || w > out->cand_w_max) out->cand_w_max = w;
            out->cand_members++;
            if (flow_own_silence(f) == 0) out->cand_unrun++;   /* the order's quantity, as the row above */
            if (s > out->cand_svc_max) out->cand_svc_max = s;
            /* The cursor, not the length: the length is constant from the instant a candidate is seeded, so it
               cannot say how far the best of them has got (flow.h). */
            if (cur > out->cand_dec_max) out->cand_dec_max = cur;
        }
    }
    /* The conservation identity the pick rows are defined by, asserted: every dispatch is credited to exactly one
       member at flow_credit_pick, a member's count starts at 0 at flow_new and no fork carries it, and flow_remove
       moves it into `g_picks_departed` on the one line that can still read it, so the halves partition the total
       and flow.h's `(picks_lifetime - picks_live) / departures` divides a numerator by its own population. Both
       asserts stand: the equality holds with a negative departed total and the inequality does not, so the pair
       separates a lost credit from a fabricated one. Over the published fields (`out->`), so the dev build and the
       document check the same three integers. */
    DCHECKF(out->picks_live + out->picks_departed == out->picks_lifetime,
            "solver/flow.c: live members hold %lld dispatches and departed members took %lld, against %lld made — "
            "flow_credit_pick credits each dispatch to one member and flow_remove moves a departing member's share, "
            "so the two must partition the total",
            (long long)out->picks_live, (long long)out->picks_departed, (long long)out->picks_lifetime);
    DCHECK(out->picks_live <= out->picks_lifetime,
           "the frontier's live members hold more dispatches than the scheduler has made — flow_credit_pick raises "
           "both and is the only writer of either");
    /* And the same for the unframed subset: raised in one statement block of flow_credit_pick (an `if` beside a
       `++`), so the inequality fails only on an edit giving one of them a second writer, which would make every
       reading of the pair about two dispatch paths. */
    DCHECK(out->unframed_picks_lifetime <= out->picks_lifetime,
           "the scheduler dispatched an unframed member more often than it dispatched at all — flow_credit_pick "
           "raises the unframed count inside the block that raises the total");
    /* And the inner one: the ready count is raised inside the outer `if`, so this fails only on an edit that lifts
       it out, which would make `readyPicksLifetime` a count over a population `unframedPicksLifetime` is not drawn
       from. */
    DCHECK(out->ready_picks_lifetime <= out->unframed_picks_lifetime,
           "the scheduler dispatched a ready job holder more often than an unframed member — the ready count is "
           "raised inside the unframed count's `if` in flow_credit_pick");
    /* The most crowded live bucket's own three numbers, from the node the walk retained (flow.h says which three
       states they separate and why three extrema cannot). This is the second reading of that bucket's membership
       pair; branch_take folded the first into a running maximum, and the identity below compares them. Once per
       census: three loads and a subtraction off a pointer in hand. NULL means no live bucket was seen, which the
       assert below says cannot happen with members standing; the rows then keep the reset's zero, and
       `br_crowd_live` tells that zero from a standing crowd never charged. */
    if (g_wfq_census_crowd) {
        const FlowAcct *cr = g_wfq_census_crowd;
        out->br_crowd_live = cr->sub_born - cr->sub_gone;
        out->br_crowd_born = cr->sub_born;
        out->br_crowd_us = cr->sub_us;
    }
    g_wfq_census_crowd = NULL;
    /* And the biggest-minting bucket's three, from the other retained node (flow.h says why the crowd's selector
       cannot reach this arm). The same cost. `sub_gone` is read directly rather than derived, so live plus shed is
       this node's `sub_born` by a path the fold never took. NULL means no live bucket was seen: `sub_born >= 1` for
       every bucket and `br_born_max` starts at 0, so the first live bucket sets both the maximum and this pointer. */
    if (g_wfq_census_minter) {
        const FlowAcct *mn = g_wfq_census_minter;
        out->br_minter_live = mn->sub_born - mn->sub_gone;
        out->br_minter_gone = mn->sub_gone;
        out->br_minter_us = mn->sub_us;
    }
    g_wfq_census_minter = NULL;
    top = flow_best();
    /* A non-empty frontier has a front. flow_best is flow_pick with no seed, no exclusion and `runnable_only` off,
       so it skips nothing; a NULL with members standing would be the pick hiding part of the frontier from its own
       maximum. Every row derived from `top` stays at zero without a top, and a zero silence notch is an ordinary
       reading for a just-forgiven leader, so the assert makes the zeros below mean only an empty frontier. The `if`
       stays because it keeps these reads off a NULL in release, where the DCHECK is compiled out. */
    DCHECK(top != NULL || g_flows_n == 0,
           "the WFQ census walked a non-empty frontier and flow_best returned no front — flow_best takes every "
           "member with no filter, so this is the pick having gained one that hides a member from its own "
           "maximum, and every row this census derives from the top would silently read as an unforgiven zero");
    if (top) {
        /* And the front of the order, in the order's quantity: the reward flow_queue_weight reads, the top flow's
           family's. `top->val` is one member's own ledger, which the pick never consults. */
        out->val_top = flow_reward(top);
        out->w_top = flow_weight(top);
        /* And the two halves of the front's silence, which `val_top` cannot say: a ledger only climbs, while the
           silence is what an emission resets, so `top_svc_fam` says whether the leading account's aging is being
           forgiven or accumulating, and `top_svc` is the half that can order members of one family (flow.h). Read
           through the aging term's notch functions, so the census and the weight share a unit. */
        out->top_svc = flow_service_notch(top);
        out->top_svc_fam = flow_family_notch(top);
        /* And how many times the front's account has had its window forgiven, the event the two notches are read
           between, which turns "the leader's aging is being reset" into a count. Read off the account, since
           flow_credit_emit is that field's only writer, once per credit (flow.h has the sweep reading with
           `picks_max`). The cast narrows the uint64 field (wide so a wrap cannot alias a stale window mark) and is
           asserted, since a row gone negative would read as a leader change. */
        DCHECK(top->family != NULL && top->family->emit_gen <= (uint64_t)INT64_MAX,
               "the leading account's forgiveness count does not fit the signed row that reports it");
        /* Guarded like its neighbours, so this is not a release-mode dereference of `top->family` guarded only by
           the DCHECK above: every other front row reaches the account through an accessor that answers 0 for a
           departed node (acct_family_val, acct_family_us, flow_own_silence), and a departed front reports the same
           0 here. The invariant is asserted one line up, where it names the site. */
        out->top_forgiven = top->family ? (int64_t)top->family->emit_gen : 0;
        /* And the identity that defines it, asked of the ledger: an account has been credited exactly when its
           generation has advanced, both ways, since flow_credit_emit raises `earned` and bumps `emit_gen` in one
           function with no return between. Asked of `earned`, not the reward (`base + earned`): an arrival is placed
           at the frontier's virtual time and holds a reward it did not emit with `emit_gen` at zero, so an identity
           over the sum would fire the first time an arrived account leads. It catches a second writer: a bump on an
           account that earned nothing, or earnings that never forgave their window. */
        DCHECK(top->family == NULL ||
               ((out->top_forgiven > 0) == (top->family->earned > 0.0)),
               "the leading account's `earned` ledger and its forgiveness count disagree about whether it has "
               "emitted — flow_credit_emit raises both and is the only writer of either (an arrival's placed reward "
               "is not `earned`)");
    }

    /* The front is one of the members this scan walked, so its silence lies inside the extrema the walk collected.
       The extrema and these readings come from two traversals of what must be one population; a leader outside its
       frontier's range is a member the walk never visited, or flow_best returning a departed flow. */
    DCHECK(!top || (out->top_svc >= out->svc_min && out->top_svc <= out->svc_max),
           "the WFQ census reports the front of the order standing outside the own-silence range its own walk "
           "collected — flow_best returns a member of the frontier this scan just enumerated, so the leader "
           "cannot be outside its extrema unless the two are reading different populations");
    DCHECK(!top || (out->top_svc_fam >= out->svc_fam_min && out->top_svc_fam <= out->svc_fam_max),
           "the front of the order's family silence lies outside the range the census walk collected — the leader's "
           "account is one this scan reached through its members");

    /* How far the job backlog stands behind the front (flow.h), taken here because it is a difference against a top
       the scan cannot know, and left at 0 when nothing waits on rank; `jobs_ready` beside it tells the two apart. */
    if (have_job_holder && top) out->job_w_gap = out->w_top - job_w_max;

    /* And the same difference for the reply backlog, left at 0.0 when nothing waits on rank, so `deliv_ready` tells
       "nobody waits on the order" from "the front holds an undelivered reply" (flow.h). */
    if (have_deliv_holder && top) {
        out->deliv_w_gap = out->w_top - deliv_w_max;
        /* And the term the gap is made of, at both ends, set in the same branch as the gap and nowhere else, so a
           non-zero gap comes with two counts about its two members and a zero gap with two zeros (flow.h). */
        out->deliv_w_gap_vis = deliv_holder->visits;
        out->w_top_vis = top->visits;
        DCHECK(out->deliv_w_gap_vis >= 0 && out->w_top_vis >= 0,
               "a WFQ census read a negative visit count at one end of the delivery gap — `visits` is raised "
               "only by flow_credit_visit and reset by nothing, so a negative one is a member whose optimism "
               "term has been computed from a count that cannot exist");
    }

    /* And the same difference for members the scheduler has never chosen, left at 0.0 when there are none, so
       `never_picked` tells "nobody is starved" from "the most-starved member stands at the front" (flow.h). */
    if (have_never && top) {
        out->never_picked_gap = out->w_top - never_w;
        /* And the plateau is the front's only when the best starved member stands on it, hence the exact equality:
           a member a hair behind the front is separated, which is the question. Left at 0 otherwise. */
        if (never_w == out->w_top) out->never_picked_at_top = never_at_w;
    }

    /* And how far the front stands ahead of the members that have run furthest through the document, one branch so
       the cursor, its population and the distance are all about members this scan found or all zero (flow.h says
       what it separates and why no term may read it). */
    if (have_cur_deep && top) {
        out->cur_deep = cur_deep;
        out->cur_deep_live = cur_deep_live;
        out->cur_deep_w_gap = out->w_top - cur_deep_w;
    }

    /* What a sample cost: this function weighs the frontier exactly twice, in its own walk and inside flow_best, so
       flow_best's weighings must equal the members enumerated. A filter added to flow_best or a `continue` added to
       the walk would make the census's price and population differ. A reader of `scanCensusWeights` alone prices a
       sample at one frontier; the second is charged to the shared OTHER entry (the host's read and the pager's tail
       run under it), which is why `other_before` brackets the call. */
    DCHECK(g_scan_weights[FLOW_SCAN_OTHER] - other_before == (long)g_flows_n,
           "the census and the flow_best inside it weighed different numbers of members — a sample weighs the "
           "frontier exactly twice, so one of the walks acquired a filter");
    /* And non-negative by the same construction: `w_top` is flow_best's maximum over every member, and a
       never-dispatched member is one, read through the same flow_weight in the same scan. The row answers whether a
       starved member is outranked or merely unchosen, which a wrong sign inverts. */
    DCHECK(out->never_picked_gap >= 0.0,
           "the census reports a never-dispatched member above the front of the queue — flow_best's maximum is over "
           "the members this scan walks, so the two are not reading one comparator");
    /* And the width counts within the gap's population: both are collected in the one branch that tests
       `picks == 0`, so a width above the count is a member the population never admitted. */
    DCHECK(out->never_picked_at_top <= out->never_picked,
           "the WFQ census reports more never-dispatched members standing at the front of the order than there "
           "are never-dispatched members at all — the width and the count are collected in one branch over one "
           "population, so they have stopped being about the same set and the plateau row is a fact about the "
           "instrument");
    /* And the two rows agree whether the starved tail touches the front: the width is non-zero exactly when the best
       starved weight is `w_top`, one comparison spelled twice, so a tolerance added to one row's notion of "at the
       front" fires here. Conditioned on a non-empty population, since both rows are zero when nothing is starved. */
    DCHECK(out->never_picked == 0 ||
           ((out->never_picked_at_top > 0) == (out->never_picked_gap == 0.0)),
           "the census's starvation width and gap disagree about whether the starved tail touches the front — the "
           "width is non-zero exactly when the best starved weight is the top");

    /* And non-negative by construction, an assertion and not a clamp: `w_top` is flow_best's maximum over every
       member, and a ready job holder is one of them read through the same flow_weight. It fires on an edit (a filter
       hiding a member from flow_best's maximum, or a second reading of the weight), either of which would make
       `job_w_gap` a number about the instrument. */
    DCHECK(out->job_w_gap >= 0.0,
           "the census reports a job backlog above the front of the queue — flow_best's maximum is over the members "
           "this scan walks, so the two are not reading one comparator");
    /* And the count and the gap are about one population: `job_w_gap` is written only under `have_job_holder`, which
       only the ready arm raises (solver/result.c's job-row banner pairs them). It catches a maximum widened to a
       framed or host-owed holder. One-sided: a non-zero count with a zero gap says the backlog's best claim stands
       at the front, the row's point. */
    DCHECK(out->jobs_ready > 0 || out->job_w_gap == 0.0,
           "the census states a job backlog's gap while counting no rank-ready job — the gap is written only for a "
           "holder the ready arm admitted");
    /* And the two kinds of ready job sum to the ready count, all three in one hand. The total accumulates the queue's
       `length` and the halves a walk of its records, so this can fail on an edit that moves one accumulation site or
       a reset that stopped covering one half. engine/build.mjs asserts it again, since this is compiled out of a
       release build its reader still runs over. */
    DCHECK(out->jobs_ready_task + out->jobs_ready_micro == out->jobs_ready,
           "the census's two kinds of rank-ready job do not sum to the ready count — the total is the queue's length "
           "and the halves a walk of its records, so the two are not one sample");
    /* And the reply backlog's gap is non-negative by the same construction. */
    DCHECK(out->deliv_w_gap >= 0.0,
           "the census reports a reply backlog above the front of the queue — flow_best's maximum is over the "
           "members this scan walks, so the two are not reading one comparator");
    /* And its count and the three fields written with it (the gap and the visit counts at its ends) are about one
       population: all three are written in the one branch `have_deliv_holder` guards, which only the ready arm
       raises, so any of them set with `deliv_ready` at zero is that branch with a second writer. */
    /* And the document-progress gap is non-negative, for `never_picked_gap`'s reason: the member it is from is one of
       the members flow_best maximises over. */
    DCHECK(out->cur_deep_w_gap >= 0.0,
           "the census reports a negative gap to the deepest member of the program table — `w_top` is the maximum "
           "over every member, including that one");
    /* And its population cannot exceed the frontier: both come out of one walk over `g_flows_n`, so this fails on a
       `continue` added above the collection or a second source of members. */
    DCHECK(out->cur_deep_live <= out->members,
           "a WFQ census counted more members standing at the deepest program row than the frontier holds — "
           "both numbers come out of one walk of the registry, so a count above the population is that walk "
           "having been given a second source of members");
    /* And the three are written together or not at all, the delivery triple's check one row over. The converse is
       not asserted: a frontier wholly at row 0 reports `cur_deep` 0 with `cur_deep_live` the whole frontier, a fresh
       document. */
    DCHECK(out->cur_deep_live > 0 || (out->cur_deep == 0 && out->cur_deep_w_gap == 0.0),
           "the census states a deepest program row or its gap with no member standing there — the cursor, its "
           "population and the gap are written in one branch");
    DCHECK(out->deliv_ready > 0 ||
           (out->deliv_w_gap == 0.0 && out->deliv_w_gap_vis == 0 && out->w_top_vis == 0),
           "the WFQ census states a reply backlog's distance from the front of the order, or the visit counts "
           "at its two ends, while counting no reply that waits on rank — all three are written in the one "
           "branch the ready arm opens, so a reading with no population under it means they have stopped "
           "being about the set `delivReady` counts");

    /* The family count is bracketed by the population it partitions: zero families with members standing is the mark
       not being taken (and would read "one family" for every frontier), and more families than members is a family
       counted twice, a stale generation aliasing a fresh node. */
    DCHECK((out->members == 0) == (out->families == 0),
           "the WFQ census counted families and members inconsistently — a frontier with members belongs to at "
           "least one family and an empty one to none, so this is the family mark not being taken, and the row "
           "that says whether the aging term's family half can order anything reads the same for every run");
    DCHECK(out->families <= out->members,
           "the WFQ census counted more fork families than there are flows to belong to them — a family is "
           "counted through its arms, so this is one node counted twice and the number is a property of the "
           "scan rather than of the frontier");

    /* The census accounts for the order it reports, which makes it a measurement of flow_weight rather than a
     * hand-kept list of fields. `w_top - w_min` is the observed spread; each term contributes at most its own
     * range, and the range of a sum is at most the sum of the ranges, so the spread is bounded by the ranges this
     * struct reports. Written over flow_weight, as flow_fork_inherit's equality is, so a term entering the weight
     * with no row here fires at the census. The slack is exactly one notch, not a tolerance: flow_silence_notch
     * floors `(cpu + fam_us)` as a sum while this scan floors the halves, and floor(a+b) <= floor(a)+floor(b)+1;
     * the 1e-9 covers the FPU. A function evaluated only inside the condition, side-effect-free, so release never
     * computes it. `top` NULL is an empty frontier, which has no spread. */
    DCHECK(!top || out->w_top - out->w_min <= wfq_accounted_spread(out) + 1e-9,
           "the WFQ census reports an order spread wider than the term ranges it reports can account for — "
           "flow_weight has a term this census cannot see, so every reading taken from it (which term is "
           "ordering the frontier, whether the aging is measuring one flow or all of them) is a statement "
           "about an instrument rather than about the run");
    /* And whether the order is deciding anything at all, which every row above presupposes: they describe the order
       over the members standing now, and a comparator schedules only a set something consumes. Lifetime counters
       (flow.h's rows), the frontier's arrival and departure processes, so `arrivals / picksLifetime` is what the
       census is conditional on. The banner at `g_arrivals` says why `rankChanges` is not this. */
    out->arrivals   = g_arrivals;
    out->departures = g_departures;
    /* And whether the order was ever offered anything to order by; the banner at `g_credit_calls` names the two
       states a zero reward band hides and why the dropped arm is correct. */
    out->credit_calls   = g_credit_calls;
    out->credit_paid    = g_credit_paid;
    out->credit_dropped = g_credit_dropped;
    /* The identity defining the partition, with all three in hand: the total is raised at flow_credit_emit's single
       entry and each arm on the line that decides it, so a third exit added later fires here. */
    DCHECK(out->credit_calls == out->credit_paid + out->credit_dropped,
           "the credit partition does not add up — flow_credit_emit's arms are supposed to be exactly two and "
           "the total is raised at its one entry, so a third exit has been added that leaves the order's own "
           "ledger unaccounted for, and `creditsDroppedLifetime` is about to be published as the count of "
           "findings no account could be given when it is a count of something narrower");
    /* The identity defining arrivals and departures, with all three terms in hand, which makes the pair checkable on
       the published document. Each has one writer (flow_new's append, flow_remove's swap-remove), so a second writer
       (a frontier trim, an eviction, a host reaching into `g_flows`) fires here. */
    DCHECK(g_arrivals - g_departures == (int64_t)out->members,
           "the frontier's arrivals minus departures do not equal its members — flow_new and flow_remove are each "
           "count's only writer, so a member joined or left `g_flows` another way");
    /* The two spellings of "unframed" are a containment, not an identity: the ready arm is reached through
       flow_stack_empty, whose first line is `if (f->frame) return 0;`, so every member it admits is inside
       `mem_unframed` (`f->frame == NULL`) by construction. What this still catches is a ready count with nothing
       unframed under it; a re-spelling of `mem_unframed` is caught per member in the walk. One-sided: `mem_unframed
       > 0` with no ready job is the second silence, the state this row was added to name (flow.h). */
    DCHECK(out->mem_unframed >= 0 && out->mem_unframed <= (long)out->members,
           "the count of members holding no frame is outside the frontier it was taken over — it is raised "
           "once per member on the same walk that sets `members`, so a figure outside [0, members] means the "
           "two are no longer one sample and the pair that separates `jobsReady`'s two zeroes is not a pair");
    DCHECK(out->jobs_ready == 0 || out->mem_unframed > 0,
           "the census reports rank-ready jobs while every member holds a frame — the ready arm is reached only "
           "through flow_stack_empty, which refuses a live frame, so `mem_unframed` was re-spelled");
    /* And the same containment for the reply backlog, which needs its own assert: a frontier holding a deliverable
       reply and no queued job (`jobsReady: 0`, `delivReady > 0`) is ordinary, and in it the job assert cannot fire
       whatever `mem_unframed` has become. */
    DCHECK(out->deliv_ready == 0 || out->mem_unframed > 0,
           "the census reports rank-ready replies while every member holds a frame — the ready arm is reached only "
           "through flow_stack_empty, which refuses a live frame, so `mem_unframed` was re-spelled");
    /* And the two identities that define the branch rows, exact because each event has one writer. Membership:
       every live member belongs to exactly one bucket (`branch` is set at flow_new and moved once, at the fork, as a
       matched departure and arrival), and the walk opens each bucket once, so the live counts partition the frontier;
       a sum below `members` is a member counted nowhere and one above it a member counted twice. Burn: every
       microsecond charged in flow_age_running lands on one bucket's `sub_us`, and a bucket is freed only once nothing
       in its subtree is live, when acct_unref folds its total into the retired term, so the thread this instance
       spent
 is attributed at every instant (`brUsLifeMax / chargedUsLife` is the concentration reading). All terms
       are published, so both are checkable on the emitted document. */
    /* The away population counted two ways: four incremental statements at four sites summed over the distinct
       families, against this walk asking flow_own_silence of every member. A difference is a transition site the
       incremental side does not have, a fifth writer of `cpu`, `cpu_gen` or `family`. Asserted, not only published,
       because the four sites were enumerated by reading writers, and an enumeration cannot state its own
       completeness. Both sides are published. */
    DCHECK(out->epoch_away_live == out->epoch_away_walk,
           "the maintained away-from-base count disagrees with a direct walk — a writer of `Flow.cpu`, "
           "`Flow.cpu_gen` or `Flow.family` outside the four maintained sites moves members across the base, or one "
           "of the four counts the wrong family (acct_depart must read before clearing either pointer)");
    DCHECK(out->br_live_sum == (long)out->members,
           "the frontier's branch buckets do not partition its members — each live member belongs to one bucket and "
           "each bucket is opened once per census");
    /* And a frontier with members standing has a thinnest bucket somebody stands in: every live member's bucket is
       taken by the member door with a live count of at least one, so this keeps wfq_accounted_spread's branch guard
       from deleting the branch term on a live frontier. The partition identity above cannot say it, since it is
       satisfied by any set of buckets whose live counts sum correctly, empty ones included. */
    DCHECK(out->members == 0 || out->br_live_min > 0,
           "the census reports members standing and no live branch bucket — every live member stands in a bucket "
           "with a live count of at least one");
    /* And the mint pair against the live pair it is folded beside, which makes `1/br_born_min - 1/br_born_max` a
       range over one population: `sub_born = live + sub_gone` with `sub_gone >= 0`, so over one set of buckets each
       end of the mint pair dominates its opposite number. The two pairs are written by two statements in
       branch_take, kept to one population only by a shared guard: a minimum over every taken bucket would let a
       departed root's single-member bucket drag `br_born_min` to 1 beneath a `br_live_min` of 2, which fires here. */
    DCHECK(out->br_born_min >= out->br_live_min && out->br_born_max >= out->br_live_max,
           "a branch bucket's lifetime mint count is below its own live membership — `sub_born` is `live` "
           "plus everything that has ever departed the bucket, so this is the mint extrema and the live "
           "extrema ranging over two different sets of buckets, and the branch term's range is about to be "
           "published as a difference between two numbers that are not two ends of one reading");
    /* And the crowd triple against the row it is selected by, which says the three describe one bucket: the left is
       a dereference of the retained node after the walk, the right a running maximum folded during it, and they agree
       only while one statement moves both. Writing the rows at the maximum's assignment would compare a statement
       with itself. */
    DCHECK(out->br_crowd_live == out->br_live_max,
           "the bucket retained as the most crowded does not hold the live maximum — the pointer and the maximum are "
           "moved by one statement in branch_take");
    /* And that bucket inside the population the other extrema ranged over, a different break: the crowd going
       through the same folds as the mint and burn extrema. A bucket reached by one door and not the others would
       make every coincidence reading ("is the crowd also the hungriest arm, the one that took most arms") compare
       two populations. One-sided, because a maximum bounds the set the crowd belongs to. (Asserted after the
       minter pair below.)
       And the minter triple against its selector, the same form for the other pointer. Written as a sum, not a
       second `born` field: live (`sub_born - sub_gone`) plus shed (`sub_gone`) reaches `sub_born` by a path the
       fold never took, and it is the one check on `br_minter_gone`, which no extremum bounds. */
    DCHECK(out->br_minter_live + out->br_minter_gone == out->br_born_max,
           "the bucket retained as the biggest minter does not hold the mint maximum — live plus shed is the "
           "lifetime mint count, and the pointer and the maximum are moved by one statement in branch_take");
    /* And that bucket inside the populations the other rows ranged over, as the crowd's pair: the live extremum and
       the held-burn accumulator, so `br_minter_us / br_held_us`, the reading the triple exists for, is a fraction
       of a denominator its numerator is drawn from. One-sided: a maximum and a sum bound sets the minter is in. */
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
    /* And the burn split, the denominator half: two accumulators in the two arms of branch_take's one condition, so
       this fails if an arm stops firing or the live guard comes apart from the live extrema's. With the identity
       below, every charged microsecond lands in one of three published places (a bucket somebody stands in, a taken
       empty bucket, a bucket whose subtree departed), checkable on a release document. */
    DCHECK(out->br_held_us + out->br_empty_us == out->br_us_sum,
           "the thread time of live and empty branch buckets does not sum to the time attributed to buckets — "
           "branch_take raises the two in the two arms of one condition");
    DCHECK(out->br_us_sum + out->br_retired_us == out->charged_us,
           "the thread time attributed to branch buckets plus the retired term does not equal the time charged — "
           "every microsecond lands on one bucket, and a freed bucket's total is folded into the retired term");
    /* And the fan against the bucket holding it, which makes `br_fan_max / br_fan_sum` a reading inside the buckets:
       every member the fan counts sits under a non-root parent, a fork off a non-root joins its parent's bucket, so
       one fan's members are live members of one bucket. The two sides have different writers (the member walk; the
       `sub_born - sub_gone` maintained at flow_fork_inherit and acct_depart), so a missed `sub_gone` or a `branch`
       re-pointed by anything but the fork separates them. An identity `br_fan_sum + <root-parented> == members` is
       not written: both terms would be raised once per trip of this loop, a loop counter compared with itself. */
    DCHECK(out->br_fan_max <= out->br_live_max,
           "a fork's live children outnumber the branch bucket they were forked into — a fork off a non-root joins "
           "its parent's bucket, so a departure left `sub_gone` unraised or something other than the fork moved "
           "`branch`");
    /* And the depth against the fan it belongs to: `br_fan_depth` 0 is how a reader is told there is no fan, and a
       real fan never stands at 0, since its node has a parent (the door tests `up->up`) and flow_fork_inherit wrote
       `depth = parent->depth + 1`. The two fields have two writers, and the compression moves `up` while leaving
       `depth`, so they can disagree. */
    DCHECK(out->br_fan_max == 0 || out->br_fan_depth > 0,
           "the census reports a deep fork at fork-tree depth 0 — a fan's node has a parent and flow_fork_inherit "
           "wrote its depth, so `up` names a node no fork attached or `depth` was not written");
}

/* The four questions, each a seed, a filter or a direction over the one scan above. */
Flow *flow_best(void) { return flow_pick(NULL, NULL, 0, 0, FLOW_SCAN_OTHER); }

/* Which flow should hold the thread: the dispatch loop's pick, defending the incumbent on a tie. */
Flow *flow_next_to_run(const Flow *incumbent, FlowScan why) { return flow_pick(incumbent, NULL, 1, 0, why); }

/* Who the incumbent defends against: the same scan with the incumbent taken out rather than seeded in, since the
   hook applies the strict comparison itself; seeded, it would answer `cur` and compare a flow with itself. */
Flow *flow_rival_of(const Flow *cur) { return flow_pick(NULL, cur, 1, 0, FLOW_SCAN_RIVAL); }

/* Is `tail` the lowest-weight candidate? The assert's own scan, independent of flow_pick, and a function because a
   DCHECK condition must be side-effect-free and is evaluated only inside the condition, so release never runs it
   (in dev it is a scan on the reclaim path, reached at the RAM floor). It calls flow_weight and nothing else, so
   the order stays single while the scan is a re-derivation. Ties pass: flow_pick may return either of two equal
   minima. */
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
    /* Every member, runnable or not, which is what eviction is about: a flow waiting on the host cannot use the
       thread, so the pick skips it, but it still occupies RAM and is the cheapest member to page, since its recipe
       re-issues the request it waits on. The one inversion is `exclude`: the flow switched in cannot be written out
       (its decision state is live in decide.c, its delta applied), so if it is the lowest the tail outranks it; this
       corrects itself at the next context switch.
       The ordering is asserted against a re-derived scan: the tail must be the lowest-weight candidate. A comparison
       against a head (min against max) cannot fail; what is at risk is the pager acquiring a ranking of its own (a
       size estimate, an age, a cheapest-to-rebuild score), and a flow_weight that is not a pure function of the
       flow, which the scan above, asking each weight once, could never notice. */
    DCHECK(!tail || flow_is_min_weight(tail, exclude),
           "the flow the pager chose to page out is not the lowest-weight member of the frontier — the tail and the "
           "head are two readings of one comparator");
    return tail;
}

void flow_remove(JSContext *ctx, Flow *f) {
    /* What this function does not free, asserted. flow_release, the one caller, releases the fields that need the
       flow switched out first (its COW delta and DOM head) and everything else a Flow owns; this releases the rest.
       A second caller (an eviction, a cancelled candidate, a frontier trim) that skipped the release would leak the
       flow's delta, DOM head, queued jobs and suspended frame with nothing naming the owner, so a field added to Flow
       gets a line in flow_release and a line here, and the skipped release crashes at the removal. */
    DCHECK(f->delta == NULL && f->dom == NULL && f->dom_base == NULL,
           "a flow was removed with its COW state still attached — flow_finish unapplies the heap delta and the DOM "
           "head before they can be freed");
    DCHECK(f->frame == NULL && f->parked == NULL,
           "a flow was removed holding a live frame or parked continuations — its whole activation chain, and "
           "everything those frames close over, would be retained by a handle nothing will ever free");
    DCHECK(JS_IsUndefined(f->jobs) && JS_IsUndefined(f->pending) && JS_IsUndefined(f->perform_q) &&
           JS_IsUndefined(f->deliver_q),
           "a flow was removed still holding queued jobs, pending host replies, unstarted cross-agent operations or "
           "routed messages — each is a work item, and the frontier may never drop one");
    /* And the one field beside them that is not work, asserted separately so its message is true (a commitment is
       nobody's work item), with the same structural half: a JSValue field added to Flow gets a line here. */
    DCHECK(JS_IsUndefined(f->deliver_world_q),
           "a flow was removed still holding the record of which sending timelines it was in — flow_release "
           "frees that Array and every [vector, taken, arm] row it names, so a flow reaching here with one was "
           "removed without being released");
    DCHECK(f->dyn == NULL && f->dyn_cand == NULL && f->dyn_type == NULL && f->dyn_url == NULL &&
           f->dyn_el == NULL && f->dyn_doc == NULL && f->dyn_id == NULL && f->dyn_run == NULL &&
           f->dyn_token == NULL && f->dyn_pos == NULL && f->dec_blob == NULL && f->pin_blob == NULL,
           "a flow was removed with its lazily-loaded chunk bodies or its suspended decision/pin blobs still "
           "attached — the flow's own allocations, freed by nothing else");
    /* The slot is read off the member (flow.h's `reg_i`), so a departure costs no scan of the frontier; the array is
       arrival order, the queue weight order, and no comparison reads a slot. The guard is a CHECK: `f->reg_i` decides
       which slot gets overwritten, a write into the registry in every build, and a swap-remove at a slot that does
       not hold this flow drops whichever member does, so the guard is data integrity in release. It reads one int
       through `f`: on a freed pointer that read is of freed memory, and every outcome but one aborts here by name;
       the one that would not (the array still holding this exact pointer at that slot) cannot arise, because the
       removal that freed it overwrote its slot. Under ASan the read is itself the report. */
    {
        int slot = f->reg_i;
        CHECK(slot >= 0 && slot < g_flows_n && g_flows[slot] == f,
              "engine: a flow was removed at a registry slot that does not hold it — a double remove, a dangling "
              "Flow*, or a slot the swap did not re-key; swap-removing there would drop another member");
        JS_FreeValue(ctx, f->fn);
        /* A routed record's queue is released in flow_release beside the operation queue, and its emptiness is
           asserted above: freeing it here would make a dropped work item invisible. */
        /* The candidate it was verifying: both strings are this flow's copies, freed nowhere else for a removed flow
           (the teardown walks the flows still there). `cand_sink` is static text. */
        free(f->cand_src); free(f->cand_payload); free(f->cand_vfy);
        /* And its place in the fork tree. Its thread time is already on the family's account (flow_age_running
           charges as it is consumed), so there is nothing to hand up; what is left is the node's lifetime, the owner
           mark coming down so a compression can walk past it, and the reference going. This is a Flow's one exit.
           The dispatches it holds move from the gauge into the counter on the one line that can still read them:
           `f->picks` is unreachable two statements below, so this cannot stand beside `g_departures++` (see the
           banner at `g_picks_departed`). */
        g_picks_departed += f->picks;
        acct_depart(f);
        free(f);
        g_flows[slot] = g_flows[--g_flows_n];   /* swap-remove; order is by weight, not position */
        /* And the one member the swap moved, re-keyed on the line that moved it. A departure renumbers exactly one
           other member; the guard is `slot < g_flows_n` because the decrement has run, so removing the last member
           self-assigns and an unguarded write would dereference the slot the freed flow occupied. */
        if (slot < g_flows_n) g_flows[slot]->reg_i = slot;
        /* And the departure counted, on the line that performs it. This decrement and the registry coming up are
           the only ways `g_flows_n` falls, and the second is asserted to happen only on a drained frontier, so the
           conservation identity is exact. */
        g_departures++;
        frontier_rank_changed();   /* frontier changed */
    }
}

int flow_count(void) { return g_flows_n; }

/* Is this pointer one of the flows standing? Answered by an address walk and never through flow.h's `reg_i`,
   because the callers ask two questions of one predicate and the stricter decides: flow_pick asks whether a
   pointer it believes live is still a member, while engine.c's hook and this file's guards ask whether a pointer
   is dangling (`g_rival` is cached across opcodes and goes stale, and flow_remove frees the flow). An address
   comparison never dereferences `f`, so it answers both; the handle would have to load `f->reg_i`, reading freed
   memory in the state the strict question exists to catch. A caller known live would get a second entry, never a
   conversion of this one. The walk checks the handle as it goes, two-sided (the field against the array) and
   after the address match, so it dereferences `f` only once the pointer is proven live. flow_pick decides its
   seed's membership inside its own scan and asserts `reg_i` for every member, so this walk is not on the
   dispatch path. */
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

/* See flow.h. The whole column, not the tail: `script_i` is where the flow has got to, and a row behind the cursor
   is a program of that document whose frames the flow may still be suspended inside. The question is whether any
   member still names this document at all, so every row counts. */
int flow_programs_for_document(uint32_t doc) {
    int n = 0;

    for (int i = 0; i < g_flows_n; i++) {
        const Flow *f = g_flows[i];
        /* A flow with no queue has no column: the parallel arrays are allocated together at the first row, so
           `dyn_n` says whether `dyn_doc` is a buffer at all. */
        for (int k = 0; k < f->dyn_n; k++)
            if (f->dyn_doc[k] == doc) n++;
    }
    return n;
}

/* Which row of `f`'s sequence is called `id` (flow.h's declaration says why it asserts nothing). A linear walk:
   it is asked once per external script reply and once per entry of a destroy walk, over a column as long as one
   document's program count, and an index beside it would be a second structure holding row identity. Zero is no
   row: `dyn_id` is minted from a counter starting at 1, so the register's default (solver/pending.h) cannot
   resolve, and an entry nobody wrote a row onto answers -1 rather than naming the first program. */
int flow_dyn_row_by_id(const Flow *f, uint64_t id) {
    DCHECK(f != NULL, "a flow's script sequence was searched for a row name with no flow to search");
    for (int k = 0; k < f->dyn_n; k++) if (f->dyn_id[k] == id) return k;
    return -1;
}

/* Is row `k` one HTML §7.5.10 "Destroying documents" step 5 takes: the criterion in one place, because the count
   and the removal below ask the same question, and two copies would let the count report a document clean while
   the removal left a row (or took one the count never promised). Every clause is argued at flow.h's declaration
   of the count. It reads three fields and writes none, so the removal can call it during its compaction, which
   writes slot `w <= k` while this is asked about `k`. */
static int prog_row_removed_by_destroy(const Flow *f, int k, uint32_t doc) {
    return f->dyn_doc[k] == doc && flow_row_unstarted(f, k);
}

/* See flow.h. */
int flow_row_unstarted(const Flow *f, int k) {
    DCHECKF(k >= 0 && k < f->dyn_n, "row %d was asked about in a sequence of %d rows", k, f->dyn_n);
    return k > f->last_compiled && k >= f->script_i;
}

/* How many of those rows lie strictly below `slot`: what an index recorded before the compaction comes down by. It
   is asked of `imm_at`/`imm_next`, which may sit one past the last row (engine.c's interposition computes a base
   of `script_i + 1` and CHECKs it against `dyn_n`), so the walk is bounded by both the slot and the column. */
static int prog_removed_below(const Flow *f, int slot, uint32_t doc) {
    int n = 0;

    for (int k = 0; k < slot && k < f->dyn_n; k++)
        if (prog_row_removed_by_destroy(f, k, doc)) n++;
    return n;
}

/* See flow.h. The cursor and `last_compiled` are both read: `script_i` is the row the flow is at and
   `last_compiled` the highest row it has started, so between programs the cursor stands one past the last started
   row and during one it stands on it. A row is unstarted exactly when it is past both: past `last_compiled`
   because a started row is a program the flow may be suspended inside, at or past `script_i` because the cursor is
   where the sequence resumes. */
int flow_programs_unstarted_for_document(const Flow *f, uint32_t doc) {
    int n = 0;

    DCHECK(f != NULL, "the unstarted-program count was asked of no flow");
    DCHECK(doc != 0, "the unstarted-program count was asked about the NONE document — a handle is this "
                     "instance's index into its own name table and zero names nothing");
    DCHECK(f->script_i >= f->last_compiled,
           "a flow's cursor is behind the last program it compiled — the compile site advances `last_compiled` to "
           "`script_i` and never past it");
    for (int k = 0; k < f->dyn_n; k++)
        if (prog_row_removed_by_destroy(f, k, doc)) n++;
    return n;
}

/* See flow.h. HTML §7.5.10 step 5 on the one task queue `JS_DropJobsForContext` cannot reach, this flow's program
 * sequence: the rows go, the ten columns shrink together, and every index into the sequence comes down with them.
 *
 * The register is walked before the columns move, because each question asked of it is which row an entry
 * names, and after the compaction that row has changed identity.
 *
 * A removed row's ownership is given back as flow_release gives it back, and the two sites are read together:
 * the body column hands a reference back (a program's text is shared by every timeline holding it,
 * solver/dyn_body.h), the address and token columns are this flow's strings and are freed, and the element column
 * frees nothing because a row's `script` element is a borrowed node. A column added to the sequence is an
 * obligation at both.
 *
 * It is this flow's sequence and nobody else's. §7.5.10 runs per timeline, so a sibling arm that has not
 * destroyed this document keeps its rows and its own naming of every shared record, and this may never write a
 * field of a shared record. Dropping this register's naming of a record is a per-flow act (pending_remove gives
 * the naming back through pending_index_unref and the record survives for whoever else names it). */
int flow_programs_remove_for_document(Flow *f, uint32_t doc) {
    int removed, lowest = -1, w = 0;

    DCHECK(f != NULL, "§7.5.10 step 5's program removal was asked of no flow");
    DCHECK(doc != 0, "§7.5.10 step 5's program removal was asked about the NONE document — a handle is this "
                     "instance's index into its own name table and zero names nothing");
    /* Through the count, so the removal cannot run over a cursor pair the count would refuse: it asserts
       `script_i >= last_compiled`, from which every index below is derived. */
    removed = flow_programs_unstarted_for_document(f, doc);
    if (removed == 0) return 0;
    for (int k = 0; k < f->dyn_n; k++)
        if (prog_row_removed_by_destroy(f, k, doc)) { lowest = k; break; }
    DCHECK(lowest >= 0,
           "§7.5.10 step 5's removal counted rows to take and then could not find one — the count and this "
           "walk ask prog_row_removed_by_destroy over the same column with the same document, so they cannot "
           "disagree unless something wrote the sequence between them");
    /* Why the cursor pair survives untouched, asserted: `lowest` is the smallest index going, so this one comparison
       establishes that no removed row lies below `script_i` or at or below `last_compiled`, and neither can name a
       different row afterwards. Widening the criterion by one clause (taking a started row, or the row at the
       cursor) would rename every index's referent while staying in range, so this fires the moment the criterion
       stops holding it. */
    DCHECK(lowest > f->last_compiled && lowest >= f->script_i,
           "§7.5.10 step 5's removal is about to take a row at or below this flow's cursor — `script_i` and "
           "`last_compiled` are absolute positions this walk does not repair; to reach those rows, bring the cursor "
           "pair down here as `imm_at`/`imm_next` are");

    /* The entries that name a row, first: engine_pending_docscript records the row's name on the register
       (PEND_SCRIPT_ROW) and flow_deliver_one_reply finds the row by it, the one structure outside this column that
       names a row. An entry whose row is going leaves with it, which §7.5.10 step 2's "Abort document." reaches:
       HTML §7.5.11 "Aborting a document load" step 2, "Cancel any instances of the fetch algorithm in the context
       of document, discarding any tasks queued for them, and discarding any further data received from the network
       for them". A reply that did land would be a destroyed Document's program compiled into a realm whose browsing
       context is null. Walked backwards because pending_remove is a swap-remove that moves the last entry into the
       hole, already visited.
       An entry whose row survives but shifts needs nothing: it names the row by `dyn_id`, the same name in every
       arm and unchanged by any shift, so the compaction carries it. A position would have needed correcting, a
       write to a record a forked sibling shares (solver/pending_index.h: one record is one member however many
       flows name it), and `pending_unshare` admits a copy only when no reply is coming, which an outstanding
       document script is not. */
    for (int k = pending_count(f->pending) - 1; k >= 0; k--) {
        JSValue e = pending_entry(f->pending, k);
        int docscript = (int)pending_get_int(e, PEND_KIND) == FLOW_PENDING_DOCSCRIPT;
        uint64_t rowid = (uint64_t)pending_get_int(e, PEND_SCRIPT_ROW);
        int slot;

        JS_FreeValue(pending_ctx(), e);
        if (!docscript) continue;
        slot = flow_dyn_row_by_id(f, rowid);
        DCHECK(slot >= 0,
               "an external document script's park names a row this flow does not hold — row names are carried by "
               "forks and compactions and retired only by this walk, so the park outlived its row");
        if (prog_row_removed_by_destroy(f, slot, doc)) { pending_remove(&f->pending, k); continue; }
    }

    /* The interposition witness is this flow's own, shared with nobody, so it is corrected arithmetically. `imm_at`
       is the base slot a run of §4.12.1.1 "Processing model"'s immediate programs was computed against and
       `imm_next` the slot the next one takes; both are absolute and at or above the cursor, as every removed row
       is. A run whose base slot is itself removed has no run left (the witness tells a second interposition it
       belongs behind the first, and the first is gone), so it returns to the "no run is open" pair rather than a
       corrected number that would claim a run nothing stands in. */
    if (f->imm_at >= 0) {
        if (f->imm_at < f->dyn_n && prog_row_removed_by_destroy(f, f->imm_at, doc)) {
            f->imm_at = f->imm_next = -1;
        } else {
            f->imm_next -= prog_removed_below(f, f->imm_next, doc);
            f->imm_at   -= prog_removed_below(f, f->imm_at, doc);
        }
    }

    /* And the columns, compacted in place. `w` never runs ahead of `k`, so the predicate is always asked about a
       slot no write has reached, and the criterion is re-asked rather than recorded into a side table. */
    for (int k = 0; k < f->dyn_n; k++) {
        if (prog_row_removed_by_destroy(f, k, doc)) {
            /* A row that owes an answer may not vanish: a non-NULL token is a peer instance suspended on this row's
               completion (flow.h: a CROSS_AGENT_OP row without one is "a peer suspended forever"), and dropping the
               row leaves the peer waiting for the session. The token is still freed below, since in release the
               only alternative is a leak. */
            DCHECK(f->dyn_token[k] == NULL,
                   "§7.5.10 step 5 removed a destroyed Document's program that still owes a cross-agent answer — a "
                   "peer is parked on this row's rendezvous token; answer it with a destroyed document's completion "
                   "before removing the row");
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
            /* The name travels with the row: its `dyn_id` does not change, so every register entry naming it still
               does. */
            f->dyn_id[w]    = f->dyn_id[k];
            /* And so does the arrival stamp: a compaction changes nothing about when a row became runnable, so a
               row older than a queued callback before §7.5.10 step 5 is older afterwards, and re-minting would let a
               destroy reorder a surviving row against this flow's own queue. */
            f->dyn_run[w]   = f->dyn_run[k];
            f->dyn_token[w] = f->dyn_token[k];
            f->dyn_pos[w]   = f->dyn_pos[k];
        }
        w++;
    }
    DCHECK(w == f->dyn_n - removed,
           "§7.5.10 step 5's compaction kept a different number of rows than the count said it would take — the "
           "count and the walk ask one predicate over one column");
    f->dyn_n = w;
    /* `script_i` and `last_compiled` are untouched; the proof is the `lowest` assert above, where the columns still
       hold the positions the claim is about. */
    DCHECK(flow_programs_unstarted_for_document(f, doc) == 0,
           "§7.5.10 step 5 left unstarted programs of the destroyed Document — the compaction uses the count's "
           "predicate, and no row can cross the cursor in this walk");
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
