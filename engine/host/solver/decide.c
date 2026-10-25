/* The frame-agnostic decision — see decide.h. */
#include "solver/decide.h"
#include "solver/concolic.h"
#include "solver/flow.h"
#include "solver/engine.h"
#include "solver/absent.h"    /* which roots are absent application state: an arm against their example assumes */
#include "solver/reclaim.h"   /* the engine's own allocations ask for a flow back before they fail */
#include "check.h"
#include "core/json_buf.h"   /* json_buf_block: the fork census's block is counted in the census's footprint */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>

static void *decide_fork_blob(int cursor, int arm, uint32_t asked);   /* the sibling's hot state (below) */
static int dec_replay(uint32_t asked);       /* the recorded arm at the cursor (below) */
/* Nothing the run observed says which arm is the real one: decide_real_arm's answer for an example-free value,
   which dec_fork_here and decide_note_forced_arm read as "make no claim". Named once because three functions
   spell it, and decide_arm already answers -1 for "not concolic" and decide_value_arm for "not decided". */
#define REAL_ARM_UNOBSERVED (-1)

/* The decision vector is a chain of cow.c's refcounted immutable segments, like CowSeg, DomSeg and ConsSeg. A
 * fork's sibling holds the parent's decisions up to the branch plus one byte, the other arm, and that prefix is
 * frozen (the cursor only moves forward, and a replayed slot is read, never written), so siblings share it
 * rather than each copying it: a copy is quadratic in the walk length, and paging it out would write the shared
 * prefix once per flow. What is per flow is the head, the decisions taken since the last freeze; everything
 * below is shared with ancestors, refcounted, and freed when the last flow standing on it goes.
 *
 * `g_c` is the cursor in whole-vector coordinates, so each segment stores `below`, the index of its first slot,
 * rather than re-deriving it. */
/* `park_id` is the segment's name in the park document, kept on the segment rather than in a pager table keyed
   by address: a partial self-park can free a segment between two runs, and the allocator may hand the same
   address to a segment on another path, which an address-keyed table would answer with the first one's
   ordinal. It is -1 until the pager assigns one, and every construction site sets it. */
/* `k` is the question each arm answers. A cold-resumed flow starts at cursor 0 and re-runs the document against
   current code and replies, so a question can appear, disappear or move; a replay that matched by position alone
   would hand each later branch another predicate's arm, in range and passing every range assert, and emit the
   same output as a clean resume. `k` makes the two distinguishable. It is a content hash of the constraint key,
   as JSStepHdr::fork_ask_key is, because the key string is freed after each ask, a rebuilt segment has no
   address, and the park document carries text; a hash can fail to catch a mismatch but never invent one.
   One allocation holds both columns, `k` first (so malloc's alignment is the uint32 array's) and `e` after it,
   so a fork costs one allocation; `e` is never freed separately, and dec_seg_unref frees `s->k`. */
typedef struct DecSeg {
    uint32_t *k;        /* the asked question per slot; owns the block, with `e` inside it */
    signed char *e;     /* the arms — points into `k`'s allocation at offset n*sizeof(uint32_t) */
    int n, below; struct DecSeg *base; int refcount; long park_id;
} DecSeg;

/* What one slot costs in the frozen chain and in the head, stated once so the census subtracts exactly what the
   allocation added. */
#define DEC_SLOT_BYTES (sizeof(uint32_t) + 1u)

static DecSeg *g_dec_base = NULL;   /* the frozen prefix the running flow stands on (NULL = it has none) */
static int g_dec_below = 0;         /* slots in that chain — the head's index origin */
/* The head is two parallel buffers, unlike a frozen segment: it is one reusable buffer for the session
   (decide_free owns it), so its two reallocs cost nothing per fork, while one growing block would re-place `e`
   on every growth. */
static signed char *g_dec = NULL; static int g_dec_n = 0, g_dec_cap = 0;   /* the head: this flow's own */
static uint32_t *g_dec_k = NULL;   /* …and which question each of its slots answers */
static int g_c = 0;
/* The names this timeline has minted at its current cursor (decide_history_name): `g_mints` of them, counted
   since the cursor stood at `g_mint_c`. Per flow, so it rides every blob a flow's decision state does. */
static int g_mint_c = 0, g_mints = 0;
static JSValue g_cur_fn = JS_UNDEFINED;   /* borrowed from the running Flow (alive for the run) */
static int g_running = 0;

/* The fork census's rows, three populations in three tables (see fork_key_count and decide.h). A mechanism row
 * is prose written at a call site in this tree (each reaches decide_fork_same_path with its caller's name), so a
 * document cannot enlarge the set; it is counted exactly in a table that grows. A constraint key is the page's
 * and unbounded, so it gets a fixed table with Space-Saving admission (Metwally, Agrawal & El Abbadi, "Efficient
 * Computation of Frequent and Top-k Elements in Data Streams", ICDT 2005): an eviction hands the departing row's
 * mass to its replacement, so counters sum to arrivals, and `n - err` is a per-row floor, so a named row can only
 * understate. Misra-Gries is rejected: its decrement-all mixes mass from held and unheld sites into one number.
 * The third population is unnamed sites, where decide_key or outcome_key answers NULL (`operand_kind` refuses an
 * Object or Symbol, whose identity is its address, and concolic_ident_compose propagates that), so the branch
 * keeps both arms and records no constraint. Its key is the value's display shape: always present, composed from
 * program facts, stable across resume, with `?` where identity went absent. It is a reporting address and must
 * never become the constraint key, which would let one flow's `a[x]` decide another's `b[x]`. It gets its own
 * fixed table, and its keys open on `~`, a byte this file writes, because the namespace is the writer's to
 * state, never the object's to sniff. */
#define DECIDE_FORK_KEYS 64
/* Smaller than the predicates' table: a predicate row names a question, and a document asks many; an unnamed
   site names a shape, and only a handful of expression forms reach it (a dynamic key read on an ordinary object,
   a derivation over one, a comparison between two). */
#define DECIDE_SITE_KEYS 32
/* One row shape for every admitted table, because Space-Saving is one algorithm. `err` is the mass inherited from
   the row displaced, so `n - err` is the floor a row may publish; a row admitted before its table filled carries
   0. */
typedef struct { char *key; long n, err; } ForkRow;
static ForkRow g_fork_keys[DECIDE_FORK_KEYS];
static ForkRow g_fork_sites[DECIDE_SITE_KEYS];
static struct { char *key; long n; } *g_fork_mech;   /* grows: bounded by this tree, never by a document */
static int  g_fork_mech_n, g_fork_mech_cap;
static long g_fork_total;

/* The replay ledger: what a replay did with the path it was handed. dec_replay consumes a recorded arm only when
   the branch's constraint key equals the key recorded beside it, which is the assertion half; this is the outcome
   half. solver/result.c's `resumed*` rows say what the document held, not what the replay did with it, and the
   `_wfq` census's `decMax`/`candDecMax` are gauges that read the same whether a flow followed or abandoned its
   recorded path, since an abandoning flow forks and regrows its vector.
   They are a report, never a verdict: a resumed flow re-derives values from current sources, so a divergence is
   permitted, and an assert on one would fire on the engine working.
   All three are lifetime counts over the session, released in decide_free as `g_fork_total` is, with the same
   lifetime as the `resumed` row read beside them; two samples of one session may be differenced. Units differ:
   `g_replay_hits` and `g_replay_left_arms` count arms (decision-vector slots), `g_replay_left` counts
   divergences, however many arms each abandoned. */
/* `g_replay_hits` nonzero with no cold resume is the ordinary state: an arm consumed here is a recorded question
   re-asked and matched, not re-execution. A forked sibling resumes over its parent's frame snapshot and an O(1)
   shared base segment (engine_sibling_assemble refuses the frameless case), so what it consumes is the arm it was
   forked to take. Read it beside `resumed`: hits with `resumed` at zero are siblings taking their own arms; with
   `resumed` above zero, the cold tier is rebuilding, and only that population re-executes anything. */
/* Forks taken over a subject this flow had already determined, an observation counted ahead of the refusal it is
   the precondition for. Concretize-on-pin reads a source this flow's equality proved back as the real value, which
   holds for the two mint arms (a bare primitive never reaches here); a source the page materialised before its
   gate keeps its concolic record (`concolic_example` leaves it intact), so in `var role = q("role"); if (role ===
   "admin") …` a second predicate such as `role.length > 3` arrives with a proved subject and forks, one arm a
   world the run contradicted. It counts forks taken (gated on `forked`), asking `concolic_src_pinned` about the
   subject's source, not the comparison result's pin bytes (concolic.h says why the two differ). A lifetime count,
   released in decide_free, reported beside `g_fork_total` in one call so `over_pinned <= total` holds of one
   moment. */
static long g_fork_over_pinned;   /* EVENTS: forks whose subject this flow had already pinned             */
/* The branch forks (dec_fork_here, the one site a predicate or an unnamed site forks at), split by whether this
   session's own observation picked one of the two arms. An observed fork's other arm is chosen and marked forced
   (decide_note_forced_arm); an unobserved fork's two arms are both chosen and neither is marked, which is the
   population a page error on such an arm is still filed as the page's from. The rest of `g_fork_total` is the
   mechanism forks (decide_fork_same_path), whose arm each mechanism states itself. Lifetime counts, released in
   decide_free. */
static long g_fork_observed;      /* EVENTS: branch forks one of whose arms the session's observation took */
static long g_fork_unobserved;    /* EVENTS: branch forks no observation of this session decided          */
static long g_replay_hits;        /* ARMS consumed on a matching question (dec_replay)                  */
static long g_replay_left;        /* EVENTS: divergences (dec_leave_path)                               */
static long g_replay_left_arms;   /* ARMS abandoned by those divergences, summed                        */

/* The third of decide_arm's three answers. A decision over a spellable question is refined from this flow's own
   constraint (no slot, no member, no cost), replayed from a recorded slot (g_replay_hits), or new (a fork,
   counted per predicate by fork_key_count, or dec_answer_here's site answer). `refined` counts the first, which
   is what a name buys. `asked` is its reachability witness, on the idiom flow.h's `index_asked` uses: `refined`
   at 0 means either no branch was re-asked inside one flow's constraint or no spellable question was reached (the
   state fork_site_name's residual describes). Both count at decide_arm only: concolic_branch_decided's other
   callers (decide_value_arm, outcome_settle) read the constraint without deciding, and must not consume a slot. */
static long g_branch_asked;       /* DECISIONS reached over a question this engine can spell (decide_arm)  */
static long g_branch_refined;     /* …of those, answered from this flow's own constraint (no slot, no fork) */

static void dec_ensure(int n) {
    if (n <= g_dec_cap) return;
    int nc = g_dec_cap ? g_dec_cap * 2 : 64;
    while (nc < n) nc *= 2;
    g_dec = reclaim_realloc(g_dec, (size_t)nc);
    CHECK(g_dec, "decide: OOM growing the decision vector — depth is bounded only by the RAM/disk floor");
    g_dec_k = reclaim_realloc(g_dec_k, (size_t)nc * sizeof *g_dec_k);
    CHECK(g_dec_k, "decide: OOM growing the decision vector's question keys — an arm whose question is not "
                   "recorded is an arm a replay cannot check itself against");
    g_dec_cap = nc;
}

/* The asked question's identity: 32-bit FNV-1a over the constraint key's bytes, as quickjs.c's step_fork_key
 * hashes an operation string for the same hazard one tier down ("A hash can only ever fail to CATCH a mismatch,
 * never invent one"). That one lives on JSStepHdr, which dies with the frame chain; this column crosses a park.
 *
 * 32 bits: the likeliest divergence is one inserted or deleted question, and the damage is done at the first
 * misaligned slot, where an 8-bit key would false-agree 1 in 256 to save 3 bytes per slot.
 *
 * Two namespaces share the column, split by DEC_KEY_SITE so a predicate's hash never equals a code site's. A
 * predicate (decide_key, outcome_key, a completion's mechanism name) clears the bit; a branch with no spellable
 * question is keyed by dec_site_hash, which sets it. A position, a pc or a display shape cannot key a predicate,
 * since that would claim knowledge the identity layer lacks. A site slot claims no predicate: it records a
 * schedule choice at a code location, which is knowledge the engine has. 0 is in neither namespace, so
 * dec_append's `asked != 0` catches a slot written with no key computed. */
#define DEC_KEY_SITE 0x80000000u

static uint32_t dec_fnv(uint32_t h, const void *bytes, size_t n) {
    const unsigned char *b = bytes;

    while (n--) {
        h ^= *b++;
        h *= 16777619u;
    }
    return h;
}

static uint32_t dec_key_hash(const char *key) {
    uint32_t h;

    DCHECK(key != NULL, "a predicate's question hash was asked of no key — a branch with no spellable question "
                        "is keyed by its code site (dec_site_hash), never by the predicate namespace");
    h = dec_fnv(2166136261u, key, strlen(key)) & ~DEC_KEY_SITE;
    return h ? h : 1u;   /* 0 is in neither namespace, so a key that lands on it is nudged off */
}

static int dec_key_is_site(uint32_t asked) { return (asked & DEC_KEY_SITE) != 0; }

/* The frozen chain's own census, counted at the two points a segment's lifetime begins and ends so the pair
   cannot drift from the thing it counts. */
static long g_dec_seg_live, g_dec_seg_entries_live, g_dec_seg_bytes_live;

/* Drop a chain reference: refcount--, free the segment at zero, continue into its base. A loop, not recursion —
   the chain's depth is the fork depth, and an unknown-length walk makes that as deep as the walk is long; the C
   stack cannot be parked. The twin of cow_seg_unref / dom_seg_unref / cons_seg_unref. */
static void dec_seg_unref(DecSeg *s) {
    while (s && --s->refcount <= 0) {
        DecSeg *base = s->base;
        /* The census's end point, matching the increments at construction, so the count is of live segments. */
        g_dec_seg_live--; g_dec_seg_entries_live -= s->n;
        g_dec_seg_bytes_live -= (long)sizeof *s + (long)((size_t)s->n * DEC_SLOT_BYTES);
        /* One free for the one block: `e` points inside `k`'s allocation (see the struct). */
        free(s->k); free(s);
        s = base;
    }
}

void decide_chain_stats(long *segs, long *entries, long *bytes) {
    if (segs) *segs = g_dec_seg_live;
    if (entries) *entries = g_dec_seg_entries_live;
    if (bytes) *bytes = g_dec_seg_bytes_live;
}

/* The running flow's decision state is over: release its chain and empty its head. `g_dec` is the one reusable
   head buffer and is not freed here; decide_free owns it. */
static void dec_clear(void) {
    dec_seg_unref(g_dec_base);
    g_dec_base = NULL; g_dec_below = 0; g_dec_n = 0;
}

static int dec_total(void) { return g_dec_below + g_dec_n; }

/* The arm recorded at whole-vector index `k`, with the question that produced it through `asked`, in one walk so
   the two columns cannot land on different slots. The head is read first, because a sibling resumes with its
   cursor at the branch it was forked over and replays the topmost slot. A cold-resumed flow starts at cursor 0
   and descends the whole chain for each read, O(depth^2) for a path of depth D: nothing is dropped, and the root
   fix is to index the chain once at decide_resume for the one running flow, never per parked flow.
   Both asserts are CHECKs because the subscript is compiled into every build, and on wasm an out-of-range load
   returns a value rather than trapping; dec_replay would then compare a garbage `asked` against the branch's
   question, and either truncate real recorded arms as a divergence or take an arm nothing recorded. The second is
   CHECK_FAIL because falling out leaves `*asked` unwritten, an uninitialised automatic in dec_replay, and a
   default would make "no slot answered" read as "the slot answered question 0". It is unreachable once the first
   holds: the bottom segment has `below == 0` (dec_seg_arm with a NULL base, or the first dec_freeze off a cleared
   head) and dec_leave_path's replacement keeps `ns->below = s->below`, so reaching it means the chain's offsets
   and the head's origin disagree. */
static int dec_at(int k, uint32_t *asked) {
    const DecSeg *s;
    CHECK(k >= 0 && k < dec_total(),
          "engine: a decision was read outside the vector — a branch would take an arm this flow never "
          "recorded. The subscript below is in every build, so this index is dereferenced in release too, and "
          "on wasm an out-of-range load returns a value instead of trapping: the replay would then either "
          "truncate the flow's real recorded arms as a divergence or step over an arm nothing chose");
    if (k >= g_dec_below) { *asked = g_dec_k[k - g_dec_below]; return g_dec[k - g_dec_below]; }
    for (s = g_dec_base; s; s = s->base)
        if (k >= s->below) { *asked = s->k[k - s->below]; return s->e[k - s->below]; }
    CHECK_FAIL("engine: a decision index below the head was not in any frozen segment — the chain's `below` "
               "offsets and the head's origin disagree, so the vector has a hole in it. Returning from here "
               "leaves the caller's `asked` UNWRITTEN, so the replay would compare the branch's question "
               "against an uninitialised automatic and take whichever arm that comparison fell out on");
}

/* Append one decision to the head with the question it answers, written together so no slot has an arm without
   a question. Every slot is a boolean arm: an operation with N feasible completions is asked as N-1 two-armed
   questions (solver_outcome), and dec_replay normalises what it reads with `? 1 : 0`, so a wider value would come
   back as a real arm nothing chose. */
static void dec_append(int arm, uint32_t asked) {
    DCHECK(arm == 0 || arm == 1,
           "a decision vector slot was written with something that is not an arm — every slot is one branch "
           "taken or not taken, and a wider value is silently narrowed by the replay's own read");
    /* Every slot carries a question a later run can re-ask: a predicate's hash or a code site's (dec_key_hash).
       A slot under one shared value would agree with every other, and the replay over them would be positional. */
    DCHECK(asked != 0,
           "a decision vector slot was recorded with no question — 0 is in neither key namespace, so this slot "
           "would agree with every other unkeyed one and a resumed flow diverging there would consume another "
           "branch's arm, counted as a replay hit with dec_leave_path never reached");
    dec_ensure(g_dec_n + 1);
    g_dec_k[g_dec_n] = asked;
    g_dec[g_dec_n++] = (signed char)arm;
}

/* An empty segment of `n` slots, counted into the census, with both columns in one block: the one speller of the
   layout for its three construction sites (a freeze, a sibling's arm, the cold tier's rebuild). The caller fills
   `k` and `e` and sets `below`, `base` and `refcount`. */
static DecSeg *dec_seg_alloc(int n) {
    DecSeg *s;

    DCHECK(n > 0, "a decision segment was allocated with no slots — the chain never freezes an empty head, so a "
                  "zero-length segment is one this file did not build and every flow standing on it would "
                  "replay its path one decision short");
    s = reclaim_malloc(sizeof *s);
    CHECK(s, "decide: OOM allocating a decision segment");
    s->k = reclaim_malloc((size_t)n * DEC_SLOT_BYTES);
    CHECK(s->k, "decide: OOM allocating a decision segment's slots");
    /* `e` lives inside `k`'s block, after the uint32 column, so `k` carries malloc's alignment; dec_seg_unref
       frees `s->k` only. */
    s->e = (signed char *)(s->k + n);
    s->n = n;
    s->park_id = -1;   /* unnamed: no park document has written this segment (see the struct) */
    g_dec_seg_live++; g_dec_seg_entries_live += n;
    g_dec_seg_bytes_live += (long)sizeof *s + (long)((size_t)n * DEC_SLOT_BYTES);
    return s;
}

/* This flow has left the path its vector describes, so the vector ends at the cursor. A replay against current
 * code and replies may diverge (and an @S candidate re-injected on a path recorded with a concolic source asks
 * fewer questions once its source is concrete); this stops it consuming arms that answer questions it did not ask.
 * Nothing is dropped or pruned: the caller forks at this branch next, so both arms run from here.
 *
 * It freezes the surviving prefix as a new segment rather than lowering `g_dec_below`, because decide_resume
 * re-derives the origin as `base->below + base->n`, and dec_seg_arm places a sibling's slot there too. Only the
 * segment the cursor falls inside is copied; the chain below is shared by pointer. The tail above is released by
 * dropping this flow's reference, and stays alive for siblings standing on it. The head is empty by
 * construction (a slot is appended only by the forking branch, which leaves the cursor at the end); asserted,
 * because zeroing it would discard decisions this flow made. */
static void dec_leave_path(void) {
    DecSeg *old = g_dec_base, *s, *ns;
    int n;

    DCHECK(g_c < dec_total(),
           "a flow was taken off its recorded path while the cursor was already at the end of it — there is "
           "nothing above the cursor to leave behind, so this call is about a divergence that did not happen");
    DCHECK(g_dec_n == 0 && g_c < g_dec_below,
           "a flow diverged from its recorded path at a cursor inside its OWN head — the head holds only "
           "decisions this run took and appended, each of which it asked for itself, so a slot there cannot "
           "answer a different question than the branch consuming it");
    /* Counted before anything moves and above the `g_c == 0` arm, which returns: a flow whose first branch
       disagreed abandons its whole vector. `dec_total() - g_c >= 1` by the assert above, so
       `replayLeftArms >= replayLeft` holds of the printed numbers. */
    g_replay_left++;
    g_replay_left_arms += dec_total() - g_c;
    flow_assumed_truncate(g_c);   /* the slots above the cursor are no longer this flow's */
    if (g_c == 0) {                      /* the very first branch disagreed: this flow stands on nothing */
        g_dec_base = NULL; g_dec_below = 0;
        dec_seg_unref(old);
        return;
    }
    for (s = old; s; s = s->base)
        if (g_c - 1 >= s->below) break;
    DCHECK(s != NULL,
           "the slot below the divergence is in no frozen segment — the chain's `below` offsets and the head's "
           "origin disagree, so the vector this flow is being truncated to has a hole in it");
    n = g_c - s->below;                  /* s's own surviving slots; everything under s survives by pointer */
    ns = dec_seg_alloc(n);
    memcpy(ns->e, s->e, (size_t)n);
    memcpy(ns->k, s->k, (size_t)n * sizeof *ns->k);
    ns->below = s->below;
    ns->base = s->base;
    ns->refcount = 1;                    /* the running flow's, and nobody else's: this prefix is new */
    /* Taken before the release, which may free `s` and cascade into the segment this one is based on. */
    if (ns->base) ns->base->refcount++;
    g_dec_base = ns;
    g_dec_below = g_c;
    dec_seg_unref(old);
}

/* Freeze the head into a shared immutable segment and hand the caller one reference on it, on top of the running
   flow's own. An empty head freezes to nothing: the caller takes another reference on the chain the flow stands
   on, so a park/resume pair with no decisions between pushes no empty segment (as concolic_pins_suspend guards),
   and the chain's depth counts forks, not switches. */
static DecSeg *dec_freeze(void) {
    DecSeg *s;

    if (g_dec_n == 0) {
        if (g_dec_base) g_dec_base->refcount++;
        return g_dec_base;
    }
    s = dec_seg_alloc(g_dec_n);
    memcpy(s->e, g_dec, (size_t)g_dec_n);
    memcpy(s->k, g_dec_k, (size_t)g_dec_n * sizeof *g_dec_k);
    s->below = g_dec_below; s->base = g_dec_base; s->refcount = 2;   /* running flow + caller */
    g_dec_base = s; g_dec_below = s->below + s->n; g_dec_n = 0;
    return s;
}

/* One more decision over `base`, as its own segment: the sibling's arm at the branch. It takes the caller's
   reference on `base` (the fork's freeze produced exactly one). `asked` is the parent's own question, since a fork
   is two arms of one branch; the sibling re-executes that branch and must compute the key recorded here. */
static DecSeg *dec_seg_arm(DecSeg *base, int arm, uint32_t asked) {
    DecSeg *s = dec_seg_alloc(1);
    DCHECK(arm == 0 || arm == 1,
           "a sibling's one recorded slot was written with something that is not an arm — see dec_append: the "
           "vector is boolean throughout, and the replay's read narrows anything wider without saying so");
    DCHECK(asked != 0,
           "a sibling's one recorded slot carries no question — see dec_append for why 0 may not enter the "
           "vector: the sibling re-asks this branch and could not check the slot against what it asks");
    s->e[0] = (signed char)arm;
    s->k[0] = asked;
    s->below = base ? base->below + base->n : 0;
    s->base = base; s->refcount = 1;   /* the fork blob's, and nobody else's until the sibling resumes */
    return s;
}

/* A fresh flow stands on nothing: an empty vector, cursor 0, and every branch it reaches is a new decision. A flow
   that carries a recorded path resumes over the chain the cold tier rebuilt, at cursor 0, so the shared chain is
   the one representation of a decision vector. */
void decide_enter(JSContext *ctx, Flow *f) {
    (void)ctx;
    dec_clear();   /* whatever the previous flow was standing on is not this one's */
    g_dec_n = 0;
    g_c = 0;
    g_mint_c = 0;
    g_mints = 0;
    g_cur_fn = f->fn;   /* borrowed */
    g_running = 1;
    concolic_clear_pins();   /* pins are per-flow: this run re-derives them as it replays its EQ gates */
}

void decide_leave(JSContext *ctx) {
    (void)ctx;
    /* The flow is done, so its reference on the chain goes with it; a finishing flow reaches neither
       decide_enter nor decide_resume, which clear before installing. */
    dec_clear();
    g_running = 0;
    g_cur_fn = JS_UNDEFINED;
}

/* The session's teardown. The head buffer and any chain the last flow left standing are this file's; the
   frontier's teardown frees the flows' blobs, which are a different reference. */
void decide_free(void) {
    int i;

    dec_clear();
    free(g_dec); g_dec = NULL;
    free(g_dec_k); g_dec_k = NULL;
    g_dec_cap = 0;   /* one capacity for both head buffers: dec_ensure grows them together */
    for (i = 0; i < DECIDE_FORK_KEYS; i++) {
        free(g_fork_keys[i].key);
        g_fork_keys[i].key = NULL; g_fork_keys[i].n = g_fork_keys[i].err = 0;
    }
    /* The unnamed sites' rows hold the page's bytes, unbounded in length and count, so they are released with
       the session too. */
    for (i = 0; i < DECIDE_SITE_KEYS; i++) {
        free(g_fork_sites[i].key);
        g_fork_sites[i].key = NULL; g_fork_sites[i].n = g_fork_sites[i].err = 0;
    }
    for (i = 0; i < g_fork_mech_n; i++) free(g_fork_mech[i].key);
    free(g_fork_mech); g_fork_mech = NULL; g_fork_mech_n = g_fork_mech_cap = 0;
    g_fork_total = 0;
    g_fork_observed = g_fork_unobserved = 0;
    /* The ledger goes with the session: it is read beside `resumed`, which describes this session's rebuild. */
    g_replay_hits = g_replay_left = g_replay_left_arms = 0;
    g_fork_over_pinned = 0;
    /* The refinement pair goes with it, for the same reason: it is read beside `replayHits` and `resumed`. */
    g_branch_asked = g_branch_refined = 0;
    /* A frozen segment has two kinds of holder: the flows forked below it (released by flow_registry_free's
       loop) and every open @S search, which freezes the path a candidate is re-injected at (solve.c's
       add_pending) and gives it back at solve_free. solver_agent_free calls solve_free before this. */
    DCHECK(g_dec_seg_live == 0,
           "the decision chain outlived the session — a frozen segment is referenced by the flows forked below "
           "it and by the @S searches that froze a re-injection path at it, and both are released before this "
           "line (flow_registry_free's loop, and solve_free from solver_agent_free), so one still live here is "
           "a blob one of those two did not give back");
}

int decide_cursor(void) { return g_c; }

/* Which predicate is growing the frontier (see decide.h). A count, not a last-seen: a frontier growing at one
 * branch forks there thousands of times and elsewhere once each, and the last fork before a sample is as likely
 * to be one of the rare ones. The table is keyed by the constraint key, so forks at one source and operation are
 * one row however many call sites spell it, and a chain (a source whose operation string carries a position)
 * shows as many one-hit rows, which is itself the answer. The admission policy is stated where the rows are
 * declared: an undercount that says so is a measurement. */
/* The row holds the whole key, because a census that truncates its key merges the rows it exists to tell apart.
   The table holds three namespaces, distinguished by first byte and asserted where every row converges: a
   constraint key is concolic_ident_compose's output, every field written `<len>:<bytes>`, so it opens on a decimal
   digit; a synthetic row is prose this file and engine.c write, opening on `(`; an unnamed site opens on
   FORK_SITE_PREFIX. A collision would add two populations into one row that still sums into the total. The
   namespace is the caller's to state, not this function's to sniff: a predicate key opening on `(` would
   otherwise pass as synthetic, so each site names the population it files into. A fork whose value this engine
   could not name is neither a predicate (nothing was asked or recorded) nor prose this tree authored. */
typedef enum { FORK_ROW_PREDICATE, FORK_ROW_SYNTHETIC, FORK_ROW_SITE } ForkRowKind;

/* The byte this file writes in front of an unnamed site's shape: the third namespace, declared once because the
   filing site and the emission assert it and engine/build.mjs partitions the object by it. It is a prefix because
   a shape is the page's bytes, which can spell `(` or a digit. */
#define FORK_SITE_PREFIX '~'

/* The census's own arithmetic, walked rather than carried: the check that the rows account for every fork is
   computed from the rows at every row that changes, not from a running sum kept beside them. Both are pure and
   vanish with the asserts. */
static long ss_sum(const ForkRow *rows, int cap)
{
    long s = 0;
    int i;

    for (i = 0; i < cap; i++) s += rows[i].n;   /* an unclaimed row holds 0 */
    return s;
}
static long fork_census_sum(void)
{
    long s = ss_sum(g_fork_keys, DECIDE_FORK_KEYS) + ss_sum(g_fork_sites, DECIDE_SITE_KEYS);
    int i;

    for (i = 0; i < g_fork_mech_n; i++) s += g_fork_mech[i].n;
    return s;
}
/* Every row's own shape. A claimed row was claimed by an arrival and only grows, so it holds at least one hit,
   and an absent row is how the table says "not seen". `err <= n - 1` is Space-Saving's guarantee as the bound the
   publisher depends on: the published floor is `n - err`, and an error reaching the count would publish a site
   this table cannot prove took a fork. */
static int ss_rows_sane(const ForkRow *rows, int cap)
{
    int i;

    for (i = 0; i < cap; i++) {
        if (!rows[i].key) { if (rows[i].n || rows[i].err) return 0; continue; }
        if (rows[i].n < 1 || rows[i].err < 0 || rows[i].err > rows[i].n - 1) return 0;
    }
    return 1;
}
static int fork_rows_sane(void)
{
    int i;

    if (!ss_rows_sane(g_fork_keys, DECIDE_FORK_KEYS)) return 0;
    if (!ss_rows_sane(g_fork_sites, DECIDE_SITE_KEYS)) return 0;
    for (i = 0; i < g_fork_mech_n; i++)
        if (!g_fork_mech[i].key || g_fork_mech[i].n < 1) return 0;
    return 1;
}
/* The lightest claimed row's count: Space-Saving's eviction candidate and the bound on the hits of every key the
   table is not holding. -1 while a row is still unclaimed, when nothing is evicted. */
static long ss_min(const ForkRow *rows, int cap)
{
    long m = -1;
    int i;

    for (i = 0; i < cap; i++) {
        if (!rows[i].key) return -1;   /* rows fill left to right: a gap means the table is not full */
        if (m < 0 || rows[i].n < m) m = rows[i].n;
    }
    return m;
}

/* Space-Saving admission (see the rows' declaration). Three cases: the row already holding this key, a row never
   claimed, and a full table, where the lightest row gives up its name and keeps its mass. */
static void ss_admit(ForkRow *rows, int cap, const char *key)
{
    int i, light = 0;
    long inherited;
    char *name;

    for (i = 0; i < cap; i++) {
        if (!rows[i].key) {                 /* never claimed: this row starts exact and stays exact */
            rows[i].key = strdup(key);
            CHECK(rows[i].key, "decide: OOM recording which predicate is growing the frontier");
            rows[i].n = 1;
            rows[i].err = 0;
            return;
        }
        if (!strcmp(rows[i].key, key)) { rows[i].n++; return; }
        if (rows[i].n < rows[light].n) light = i;
    }
    /* The mass is inherited and the name is not. The displaced key's hits stay in the table, so the rows still sum
       to the forks, and they are recorded as the new row's error, so they are never published as forks at the key
       that displaced it. */
    inherited = rows[light].n;
    DCHECK(inherited == ss_min(rows, cap),
           "a fork census eviction displaced a row that is not the lightest — Space-Saving bounds a row's error "
           "by the count of the row it displaced, so displacing anything heavier makes every floor this table "
           "publishes larger than the algorithm can prove and the census overstates a site");
    name = strdup(key);
    CHECK(name, "decide: OOM recording which predicate is growing the frontier");
    free(rows[light].key);
    rows[light].key = name;
    rows[light].n   = inherited + 1;
    rows[light].err = inherited;
}

/* The mechanism rows, exact, in a table that grows (see the declaration). The strings are this tree's, so a
   document cannot add one, and a fixed size with an overflow arm would bound a set no page can enlarge. */
static void fork_mech_count(const char *key)
{
    int i;

    for (i = 0; i < g_fork_mech_n; i++)
        if (!strcmp(g_fork_mech[i].key, key)) { g_fork_mech[i].n++; return; }
    if (g_fork_mech_n == g_fork_mech_cap) {
        int nc = g_fork_mech_cap ? g_fork_mech_cap * 2 : 8;
        void *p = realloc(g_fork_mech, (size_t)nc * sizeof *g_fork_mech);
        CHECK(p, "decide: OOM recording which mechanism is growing the frontier");
        g_fork_mech = p; g_fork_mech_cap = nc;
    }
    g_fork_mech[g_fork_mech_n].key = strdup(key);
    CHECK(g_fork_mech[g_fork_mech_n].key, "decide: OOM recording which mechanism is growing the frontier");
    g_fork_mech[g_fork_mech_n].n = 1;
    g_fork_mech_n++;
}

static void fork_key_count(const char *key, ForkRowKind kind)
{
    DCHECK(key != NULL && *key,
           "a fork census row was filed under no name at all — every row of this table is either a constraint "
           "key or a named mechanism that forks without one, and an empty name merges with nothing because it "
           "IS nothing: the row would sum into the total while describing no site a reader can go to");
    /* A constraint key opens on its own length prefix: concolic_ident_compose writes `<len>:<bytes>` per field, so
       `branch` composes to a key beginning "6:" and `outcome` to "7:". If this fires, the identity grammar changed
       and the prose rows can collide with it. */
    DCHECK(kind != FORK_ROW_PREDICATE || (*key >= '0' && *key <= '9'),
           "a fork census row was filed as a PREDICATE under a name that is not a constraint key — every key "
           "concolic_ident_compose produces opens on a decimal length prefix, and a name that does not is "
           "either prose filed through the wrong site or an identity grammar that no longer keeps the two "
           "namespaces disjoint. Either way the synthetic rows can now collide with a predicate and the "
           "largest row of this table stops naming a site");
    /* A synthetic row opens on `(`, so a mechanism row is never read as a branch the program took. */
    DCHECK(kind != FORK_ROW_SYNTHETIC || *key == '(',
           "a mechanism that forks without a predicate filed its row under a name that does not read as one — "
           "the prose rows are told apart from constraint keys by their leading `(` and by nothing else, so a "
           "name without it is a fabricated predicate: it merges with a real key the moment one spells the "
           "same way, and until then it reports a branch the program never took");
    /* An unnamed site is checked by neither: its name is the page's display shape, which can open on any byte. Its
       namespace is made at the emission, where FORK_SITE_PREFIX is written (see fork_site_name and
       decide_fork_json). Here the caller's kind picks the table, so the three populations cannot collide however a
       key is spelled; the asserts check that a site files into the population it thinks it is. */
    g_fork_total++;
    if (kind == FORK_ROW_PREDICATE) ss_admit(g_fork_keys,  DECIDE_FORK_KEYS, key);
    else if (kind == FORK_ROW_SITE) ss_admit(g_fork_sites, DECIDE_SITE_KEYS, key);
    else                            fork_mech_count(key);
    DCHECK(fork_rows_sane(),
           "a fork census row is a shape this table cannot publish — every claimed row holds at least the one "
           "arrival that claimed it, and its inherited error is strictly below its count so the floor it "
           "publishes (`n - err`) names at least one fork. A row outside that is a census that can report a "
           "site as having taken no forks, which is a site the reader will go and look for");
    DCHECK(fork_census_sum() == g_fork_total,
           "the fork census rows stopped accounting for the forks counted into them — Space-Saving conserves "
           "mass by construction (a hit, a claim and an eviction each add exactly one), so a divergence here "
           "is a fork counted into the total and filed into neither table, and the frontier's provenance "
           "identity `created = 1 + forks + …` no longer closes over anything a reader can subtract");
}

/* The name of a fork this engine could not spell a question for: the value's display shape, borrowed from the
 * record (the census strdups what it admits; decide_fork_json writes the `~` byte). It answers where, never
 * what: it is never handed to concolic_constrain_branch, since `a[x]` and `b[x]` over two ordinary objects
 * compose one shape ("Absence costs forks; a wrong identity costs the arm."), and the question column takes it
 * only beside the branch's code site (dec_site_hash). The shape is total (concolic_alloc strdups `{}` for a value
 * handed none), so the DCHECK asserts it; it is borrowed, so this class of fork allocates no key.
 *
 * Named residual: the operand has no name. Not covered: `Symbol("x")`, `[]` and the exotic-state classes
 * enumerated at concolic.c's literal_ident; a branch over one records no constraint and re-forks at each reach,
 * because an address is reused and survives no park.
 * Next diff builds: creation names for those classes, beside JS_CreationName's (site, creating flow's ordinal),
 * JS_IntrinsicName and JS_SymbolRegistryKey, after which decide_key stops answering NULL and the fork moves to the
 * predicate rows (engine/tests/solver/unknown_key_*.html state the shapes). How its absence shows: on one
 * document a `~` row grows while `branchRefined` stays flat; a working namer lowers the site row and raises
 * `branchRefined`. */
static const char *fork_site_name(JSValueConst subject) {
    const char *sh = concolic_shape_c(subject);

    DCHECK(sh != NULL,
           "a fork with no spellable question was asked to name its site over a value that is not a concolic "
           "— every value this seam decides over is one (decide_branch and solver_outcome both assert it), and "
           "every concolic carries a display shape because its mint copies `{}` for a value handed none, so a "
           "NULL here is a decision reached over something that never went through that mint");
    return sh;
}

/* The question a branch with no spellable predicate records, in DEC_KEY_SITE's namespace: its code site and its
   subject's shape. The code site is where the page's code stands (JS_RunningSiteHash: the body's cross-session
   locator, which folds the bundle's content digest, plus the byte offset of the opcode), the asking C site by
   basename and line, and, for an outcome walk, the operation and the completion asked about. No page frame
   standing (host time, a restartable fork's re-run) folds its own tag, never a reserved hash, so the C site
   alone names it. The offset reproduces within one build (JS_RunningSiteHash says why), so a park resumed by
   another build meets a different key here and diverges through dec_leave_path. */
static uint32_t dec_site_hash(JSContext *ctx, JSValueConst subject, const char *op, int completion,
                              const char *site) {
    uint32_t h = 2166136261u;
    uint64_t at;
    unsigned char tag, b[8];
    const char *base, *shape;
    int i;

    DCHECK(site != NULL, "a branch with no spellable question reached the site key with no ask site — "
                         "SOLVER_SITE_HERE is captured at every ask and threaded, so NULL is a caller that "
                         "dropped it");
    tag = JS_RunningSiteHash(ctx, &at) ? 1 : 0;
    h = dec_fnv(h, &tag, 1);
    if (tag) {
        for (i = 0; i < 8; i++) b[i] = (unsigned char)(at >> (56 - 8 * i));
        h = dec_fnv(h, b, sizeof b);
    }
    base = strrchr(site, '/');
    base = base ? base + 1 : site;
    h = dec_fnv(h, base, strlen(base) + 1);
    tag = op ? 1 : 0;
    h = dec_fnv(h, &tag, 1);
    if (op) {
        h = dec_fnv(h, op, strlen(op) + 1);
        for (i = 0; i < 4; i++) b[i] = (unsigned char)((uint32_t)completion >> (24 - 8 * i));
        h = dec_fnv(h, b, 4);
    }
    shape = fork_site_name(subject);
    h = dec_fnv(h, shape, strlen(shape));
    return h | DEC_KEY_SITE;
}

/* The census serializes itself, as solver/result.c's rule says for every surface (endpoint.c its endpoints,
   solve.c its sinks): a constraint key carries control-character separators, so the escaping belongs with the
   table. It rides the result document as `_forkAt`. A predicate row publishes its floor `n - err`; the inherited
   mass is published once, summed, as an overflow row under prose no key can spell, so mechanism rows, floors and
   overflow partition the counters and still sum to the forks. The overflow row is absent exactly when nothing
   was evicted, and bounds how far any named row understates. Each table also publishes, as a `_`-prefixed member
   rather than a row (it is a bound on hits, not hits, and must not enter the sum), Space-Saving's bound on a key
   it is not holding: the lightest resident count, which separates an evicted site from one never reached. It is
   present on every object with rows, with value 0 when nothing was excluded, so its absence means a producer
   that predates it.
   Residual: each row's own error is not emitted. Next diff builds `key -> [floor, err]`, which
   extension/bridge.js's finite-number DCHECK and popup.js's `key x N` renderer refuse, so it lands with them. Its
   absence shows as exact rows quoted as though each might understate by the whole spill. Caller frees. */
char *decide_fork_json(void)
{
    static const char OVERFLOW_KEY[] = "(forks the named rows cannot prove are their own)";
    /* Space-Saving's bound on every key this table is not holding. It is named as a sentence because
       extension/popup.js renders the object generically as `key x N`, and a terse name would read as a fork site.
       It is this file's literal, written unescaped; engine/build.mjs reads it to tell the member from a row and
       refuses an escaped spelling. */
    static const char LIGHTEST_KEY[] = "_the most any site this table is not holding can have taken";
    /* The unnamed sites' own pair, since a bound is about the one table that did the evicting. The overflow row
       opens on `(`, not the site prefix: it is the mass of sites the table cannot name, matched by name before
       any byte test, as decide.h states for its twin. */
    static const char SITE_OVERFLOW_KEY[] =
        "(forks at unnamed sites the named shapes cannot prove are their own)";
    static const char SITE_LIGHTEST_KEY[] =
        "_the most any unnamed fork site this table is not holding can have taken";
    size_t n = 3;   /* "{}" and the NUL */
    char *out;
    size_t len = 0;
    /* Members, not rows: the count places the separating commas, and this object has members that are not
       rows. */
    int i, phase, members = 0;
    long spill = 0, sum = 0, lightest;
    long site_spill = 0, site_lightest;

    /* The size is counted from the rows, since a key is the page's bytes: each byte can escape to six
       (`\u00XX`), a row costs two quotes, the colon, the comma and a long's widest 20 digits, and the object its
       braces. */
    for (i = 0; i < DECIDE_FORK_KEYS; i++)
        if (g_fork_keys[i].key) { n += strlen(g_fork_keys[i].key) * 6 + 24; spill += g_fork_keys[i].err; }
    /* A site row costs one byte more than its key, for the `~` written here (fork_site_name says why); it fits in
       the 24 bytes every row carries. */
    for (i = 0; i < DECIDE_SITE_KEYS; i++)
        if (g_fork_sites[i].key) { n += strlen(g_fork_sites[i].key) * 6 + 24; site_spill += g_fork_sites[i].err; }
    for (i = 0; i < g_fork_mech_n; i++) n += strlen(g_fork_mech[i].key) * 6 + 24;
    if (spill) n += sizeof OVERFLOW_KEY * 6 + 24;
    if (site_spill) n += sizeof SITE_OVERFLOW_KEY * 6 + 24;
    if (g_fork_total) n += sizeof LIGHTEST_KEY * 6 + 24 + sizeof SITE_LIGHTEST_KEY * 6 + 24;
    out = json_buf_block(n);
    if (!out) return NULL;
    out[len++] = '{';
    /* The bound first, before the rows it qualifies, on every object that has rows, so its absence beside rows
       means only a producer that predates it. `spill == 0` means every key seen is resident (an eviction leaves
       `inherited >= 1` that no admission clears), so the bound is exactly 0; above zero it is Space-Saving's,
       which holds over the full table a non-zero spill proves. `{}` still says this document never forked. */
    lightest      = spill      ? ss_min(g_fork_keys,  DECIDE_FORK_KEYS) : 0;
    site_lightest = site_spill ? ss_min(g_fork_sites, DECIDE_SITE_KEYS) : 0;
    if (g_fork_total) {
        DCHECK(lightest >= 0,
               "the fork census has mass it cannot attribute and yet reports an unclaimed predicate row — an "
               "eviction is the only thing that produces that mass and it only ever happens on a FULL table, "
               "so this pair says a row was displaced out of a table that still had a free slot. The bound "
               "about to be published is Space-Saving's, and it holds only over a table where every slot is "
               "claimed: published off a partial one it would understate what an excluded key can have taken "
               "and a reader would call an argmax safe that is not");
        DCHECK((lightest > 0) == (spill > 0),
               "the fork census's bound on its absent keys disagrees with its own overflow row about whether "
               "anything was ever excluded — those two are one fact stated twice (a spill is the residue of "
               "an eviction and an eviction is the only way a key leaves this table), so a reader that trusts "
               "one is contradicted by the other and the pair no longer separates `evicted` from `never seen`");
        DCHECK(LIGHTEST_KEY[0] == '_',
               "the fork census's own bound is named in a namespace a ROW can spell — a consumer tells this "
               "member from the rows by that leading `_` and by nothing else, so a name without it is summed "
               "into the forks as though it were mass, and every percentage this object supports is then "
               "taken against a denominator no fork produced");
        members++;
        len += (size_t)snprintf(out + len, n - len, "\"%s\":%ld", LIGHTEST_KEY, lightest);
        DCHECK(len < n, "the fork census overran the size counted for its own bound — that member's key is "
                        "this file's own literal and was priced from it, so it does not fit only if the "
                        "pricing above stopped being over the same string");
        /* The unnamed sites' bound, on every object with rows, for its twin's reason; the two DCHECKs are made
           again over this table, which has its own history. */
        DCHECK(site_lightest >= 0,
               "the fork census has unattributable mass among its UNNAMED SITES and yet reports an unclaimed "
               "site row — an eviction only ever happens on a FULL table, so this pair says a shape was "
               "displaced out of a table that still had a free slot, and the bound about to be published "
               "holds over a full table and nothing else");
        DCHECK((site_lightest > 0) == (site_spill > 0),
               "the fork census's bound on the unnamed sites it is not holding disagrees with that table's "
               "own overflow row about whether anything was ever excluded — one fact stated twice, so a "
               "reader trusting either is contradicted by the other and the pair stops separating an EVICTED "
               "shape from one this document never reached");
        DCHECK(SITE_LIGHTEST_KEY[0] == '_',
               "the unnamed-site bound is named in a namespace a ROW can spell — a consumer tells a member "
               "from a row by that leading `_` and by nothing else, so a name without it is summed into the "
               "forks as mass no fork produced");
        members++;
        out[len++] = ',';
        len += (size_t)snprintf(out + len, n - len, "\"%s\":%ld", SITE_LIGHTEST_KEY, site_lightest);
        DCHECK(len < n, "the fork census overran the size counted for the unnamed sites' bound — that "
                        "member's key is this file's own literal and was priced from it");
    }
    for (phase = 0; phase < 5; phase++)
    for (i = 0; i < (phase == 0 ? g_fork_mech_n :
                     phase == 1 ? DECIDE_FORK_KEYS :
                     phase == 3 ? DECIDE_SITE_KEYS : 1); i++) {
        const char *k;
        long hits;
        /* The one phase whose namespace is made here rather than carried on the key (see fork_site_name). */
        int prefixed = (phase == 3);

        if (phase == 0) {                               /* the mechanisms: exact, and no error to publish */
            k = g_fork_mech[i].key; hits = g_fork_mech[i].n;
        } else if (phase == 1) {                        /* the predicates: the floor, never the counter */
            if (!g_fork_keys[i].key) continue;
            k = g_fork_keys[i].key; hits = g_fork_keys[i].n - g_fork_keys[i].err;
        } else if (phase == 2) {
            if (!spill) continue;
            k = OVERFLOW_KEY; hits = spill;
        } else if (phase == 3) {                        /* the unnamed sites: the floor, on its own table */
            if (!g_fork_sites[i].key) continue;
            k = g_fork_sites[i].key; hits = g_fork_sites[i].n - g_fork_sites[i].err;
        } else {
            if (!site_spill) continue;
            k = SITE_OVERFLOW_KEY; hits = site_spill;
        }
        sum += hits;
        /* The namespace, asserted where the consumer reads it: a reader splitting predicates from fork sites
           tests exactly this first byte, so a fourth spelling would be read as one of the three. */
        DCHECK(prefixed || *k == '(' || (*k >= '0' && *k <= '9'),
               "the fork census is emitting a row whose name opens on none of its three namespaces — a "
               "constraint key opens on concolic_ident_compose's decimal length prefix, a mechanism row and "
               "the two overflow rows on `(`, and an unnamed fork site on the prefix written one line below "
               "this; the object is partitioned by that byte alone. A row outside all three is read as "
               "whichever of them its first byte happens to resemble, which files a mechanism's exact count "
               "among the predicate floors, or a page's shape among rows a document cannot grow");
        DCHECK(!prefixed || FORK_SITE_PREFIX != '(',
               "the unnamed-site prefix has become a byte one of the other namespaces already owns — the "
               "three populations are told apart by that byte and nothing else, so this would file every "
               "shape a document forked at among the rows a document cannot grow");
        if (members++) out[len++] = ',';
        out[len++] = '"';
        /* The namespace byte, written inside the quotes and before the escaping loop, because it is this file's
           literal, not the page's. */
        if (prefixed) out[len++] = FORK_SITE_PREFIX;
        for (; *k; k++) {
            if (*k == '"' || *k == '\\') { out[len++] = '\\'; out[len++] = *k; }
            else if ((unsigned char)*k < 0x20) len += (size_t)snprintf(out + len, n - len, "\\u%04x",
                                                                      (unsigned char)*k);
            else out[len++] = *k;
        }
        out[len++] = '"';
        out[len++] = ':';
        len += (size_t)snprintf(out + len, n - len, "%ld", hits);
        DCHECK(len < n, "the fork census overran the size its own rows were counted from — the count above is "
                        "over the same table this loop walks, so a row that does not fit is a key that grew "
                        "between the two passes or an escape wider than the six bytes priced for it");
    }
    out[len++] = '}';
    out[len] = 0;
    DCHECK(len < n, "the fork census overran its buffer at the closing brace — a truncation here does not lose "
                    "a row, it loses the brace, so the document that embeds it will not parse and every "
                    "finding for this page is discarded");
    DCHECK(sum == g_fork_total,
           "the fork census object's rows do not sum to the forks it counted, and the object is where that "
           "identity is actually READ — a consumer takes the total off these rows, so a partition that leaks "
           "hands back percentages of a denominator that is not the number of forks. The five phases are a "
           "partition of Space-Saving's counters over BOTH admitted tables (a mechanism's exact count, a "
           "predicate's floor `n - err`, that table's errors summed once, an unnamed site's floor, and that "
           "table's errors summed once), so a mismatch is a phase that skipped a claimed row or "
           "double-counted an error. `sum` is over ROWS ONLY and the bound member is deliberately outside it — it is a bound "
           "on hits and not a count of them, and adding it here would make this identity close over mass no "
           "fork produced, which is the one way the check could pass while the object it checks is wrong");
    return out;
}

long decide_fork_total(void) { return g_fork_total; }

/* See decide.h. The total is reported with the pinned count in one call, so `over_pinned <= total` holds of one
   moment rather than of two getters read around a fork. */
void decide_fork_observation_stats(long *total, long *observed, long *unobserved, long *mechanism) {
    long mech = 0;
    int i;

    DCHECK(total && observed && unobserved && mechanism, "the fork observation census was asked to fill nothing");
    for (i = 0; i < g_fork_mech_n; i++) mech += g_fork_mech[i].n;
    /* The partition: every fork is filed at dec_fork_here (counted above by its observation) or at
       decide_fork_same_path (a mechanism row), and at no third site. */
    DCHECK(g_fork_observed + g_fork_unobserved + mech == g_fork_total,
           "the fork census's three populations do not sum to its total — a fork was counted into the total "
           "at a site that is neither dec_fork_here nor decide_fork_same_path, so the observation split "
           "states a denominator it does not cover");
    *total = g_fork_total;
    *observed = g_fork_observed;
    *unobserved = g_fork_unobserved;
    *mechanism = mech;
}

void decide_fork_pinned_stats(long *total, long *over_pinned) {
    if (total) *total = g_fork_total;
    if (over_pinned) *over_pinned = g_fork_over_pinned;
}

/* See decide.h. The three are handed back in one call because the identity over them holds of one moment: three
   getters would let a caller sample `left` before a divergence and `leftArms` after it and publish a violation
   the engine never held. */
void decide_replay_stats(long *hits, long *left, long *left_arms) {
    if (hits) *hits = g_replay_hits;
    if (left) *left = g_replay_left;
    if (left_arms) *left_arms = g_replay_left_arms;
}

/* See decide.h. Both in one call, for decide_replay_stats' reason: `refined <= asked` holds of one moment. */
void decide_refine_stats(long *asked, long *refined) {
    if (asked) *asked = g_branch_asked;
    if (refined) *refined = g_branch_refined;
}

/* Swap the running decision state when the scheduler interleaves flows. A paused flow keeps its whole vector and
   its cursor, so on resume it consumes the same decisions from where it left off; decide_suspend snapshots,
   decide_resume restores and re-binds cur_fn. The cold tier rebuilds a parked flow into the same shape. A blob is
   a pointer at a segment and a cursor, O(1) whatever the vector's depth. Every arm a flow owes is a slot in its
   chain under the question it answers, so the chain and cursor are the whole of what crosses the cold tier. */
typedef struct { DecSeg *seg; int c; int mint_c, mints; } DecideBlob;
/* What every blob a fork or suspend takes from the running flow besides its chain: the mint count at the cursor,
   which the sibling shares because it shares the history up to here. */
static void dec_blob_take_mints(DecideBlob *b) { b->mint_c = g_mint_c; b->mints = g_mints; }
void *decide_suspend(void) {
    DecideBlob *b = reclaim_malloc(sizeof *b); CHECK(b, "decide: OOM suspend blob");
    b->seg = dec_freeze();   /* the blob owns the reference the freeze hands back */
    b->c = g_c;
    dec_blob_take_mints(b);
    return b;
}
void decide_resume(void *blob, JSValueConst fn) {
    DecideBlob *b = blob;
    DCHECK(b != NULL, "a flow was resumed with no parked decision state — every branch it had already taken "
                      "would be re-decided, and it would re-fork every sibling it has already produced");
    dec_clear();                 /* the head and chain belong to the flow that just parked */
    g_dec_base = b->seg;
    if (g_dec_base) { g_dec_base->refcount++; g_dec_below = g_dec_base->below + g_dec_base->n; }
    g_c = b->c;
    DCHECK(g_c <= dec_total(), "a flow resumed with a cursor past the end of its own decision vector — the "
                               "next branch would read a slot nothing recorded");
    DCHECK(b->mint_c >= 0 && b->mint_c <= g_c && b->mints >= 0,
           "a flow resumed with a mint count taken at a cursor past its own — dec_blob_take_mints copies both "
           "from the running flow, whose cursor only moves forward");
    g_mint_c = b->mint_c;
    g_mints = b->mints;
    g_cur_fn = fn;   /* borrowed from the resuming Flow */
    g_running = 1;
}
void decide_blob_free(void *blob) { DecideBlob *b = blob; if (b) { dec_seg_unref(b->seg); free(b); } }

/* The cold tier's read and write of this chain (decide.h says why the segments, not a flattened vector, cross).
   The pointers are opaque to the caller, which names a segment by a park-local ordinal it assigns. */
const void *decide_blob_seg(const void *blob) {
    const DecideBlob *b = blob;
    DCHECK(b != NULL, "the cold tier asked for the decision chain of a flow that has none parked — a flow that "
                      "has run is switched OUT before the frontier is walked, so a missing blob here means the "
                      "walk is reading a flow the scheduler still holds");
    return b->seg;
}

const void *decide_seg_base(const void *seg) {
    DCHECK(seg != NULL, "the cold tier walked below the bottom of a decision chain — the walk ends at the "
                        "segment whose base is NULL, and asking that one for a base again is a walk that "
                        "cannot terminate");
    return ((const DecSeg *)seg)->base;
}

int decide_seg_arms(const void *seg, const signed char **arms) {
    const DecSeg *s = seg;
    DCHECK(s != NULL, "the cold tier asked for the arms of a segment that does not exist");
    DCHECK(s->n > 0, "a frozen decision segment holds no arms — dec_freeze refuses an empty head and an arm "
                     "segment is exactly one, so a zero-length one is a chain this file did not build");
    *arms = s->e;
    return s->n;
}

const uint32_t *decide_seg_keys(const void *seg) {
    const DecSeg *s = seg;
    DCHECK(s != NULL, "the cold tier asked which questions a segment's arms answer, for a segment that does "
                      "not exist");
    return s->k;
}

void *decide_seg_new(void *base, const signed char *arms, const uint32_t *keys, int n) {
    DecSeg *b = base, *s;

    DCHECK(n > 0, "a decision segment was rebuilt with no arms — every flow standing on it would replay its "
                  "path one decision short, and every branch after that would read the next flow's answer");
    DCHECK(keys != NULL,
           "a decision segment was rebuilt from a park document that recorded arms but not the questions they "
           "answer — a replay over it could not tell that it had left the recorded path, which is the whole "
           "reason the column crosses the park boundary. The document predates the key column; it is residue "
           "from a writer that could not have written one, not a document to read arms out of");
    s = dec_seg_alloc(n);
    memcpy(s->e, arms, (size_t)n);
    memcpy(s->k, keys, (size_t)n * sizeof *keys);
    s->below = b ? b->below + b->n : 0;
    s->base = b;
    /* One reference for the base link, as a freeze transfers one, and one for the rebuilder, which holds every
       segment it made until the whole park is decoded, because a segment's users are decoded after it. */
    if (b) b->refcount++;
    s->refcount = 1;
    /* A rebuilt segment is unnamed (dec_seg_alloc set -1): the ordinal a resume reads belongs to the document it
       read, while `park_id` names the segment in the document this session will write, so ordinals stay dense in
       one namespace. */
    return s;
}

void decide_seg_release(void *seg) { dec_seg_unref(seg); }

long decide_seg_park_id(const void *seg) {
    const DecSeg *s = seg;
    DCHECK(s != NULL, "the cold tier asked for the park name of a segment that does not exist");
    return s->park_id;
}

void decide_seg_set_park_id(const void *seg, long id) {
    DecSeg *s = (DecSeg *)seg;
    DCHECK(s != NULL, "the cold tier named a segment that does not exist");
    DCHECK(id >= 0, "a decision segment was given a negative park name — -1 is what says a segment is NOT in "
                    "the document, so writing one as a name makes an emitted segment indistinguishable from an "
                    "unemitted one and the next walk emits it a second time");
    DCHECK(s->park_id < 0,
           "a decision segment was named TWICE in one park document — the pager writes each segment once and "
           "the ordinals are dense, so a second name means the walk re-emitted a segment it had already "
           "written, and the flows standing on the two ordinals now share a chain that was written twice");
    s->park_id = id;
}

void *decide_blob_new(void *seg) {
    DecideBlob *b = reclaim_malloc(sizeof *b);
    CHECK(b, "decide: OOM rebuilding a parked flow's decision state");
    b->seg = seg;
    if (seg) ((DecSeg *)seg)->refcount++;
    /* Cursor 0 makes this a replay: the flow re-runs the document and consumes one recorded arm at each branch it
       re-reaches, and forks like any other flow once the cursor reaches the chain's end. It has minted nothing
       yet, so it re-mints every name its recorded run minted (decide_history_name). */
    b->c = 0;
    b->mint_c = 0;
    b->mints = 0;
    return b;
}

/* See decide.h. `entries` is how many decisions the flow stands on, a property of the chain shared with every
   ancestor; `bytes` is what the flow itself owns, the blob. Together they show the sharing. `entries` is where the
   recorded path ends, not how far the flow has got (decide_blob_cursor answers that): for a flow installed by
   decide_blob_new it is a whole path under a cursor of 0, fixed from the instant the blob is built. The parked
   cursor is asserted here as well as at decide_resume, because the frontier's census walks every parked blob, so
   a cursor that has come apart from its chain is caught while the member holding it can be named. */
void decide_blob_stats(const void *blob, long *entries, long *bytes) {
    const DecideBlob *b = blob;
    long n;

    /* An absent blob and a flow standing on nothing are different facts: 0 is the commonest true answer, so a
       default here would be invisible. CHECK, because the dereference below is in every build. */
    CHECK(b != NULL,
          "decide: the decision state of a flow that has none parked was asked for — a running flow's state is "
          "live (decide_live_stats answers that one) and a parked flow's is its blob, so a NULL here is a "
          "caller reading neither and about to record the answer as a flow standing on zero decisions");
    n = b->seg ? b->seg->below + b->seg->n : 0;
    DCHECK(b->c >= 0 && b->c <= n,
           "a parked flow's decision cursor is outside its own chain — the cursor names the slot the flow "
           "replays next and the chain is every slot it has, so on the resume this blob feeds decide_resume "
           "would install a position with no arm under it and the next branch would read a slot nothing "
           "recorded, or step over arms this flow really did take");
    if (entries) *entries = n;
    if (bytes) *bytes = (long)sizeof *b;
}
/* The cursor: how far along its chain a parked flow has got (see decide.h), which decide_blob_stats' `entries` is
   not. The range is asserted here too, because the census and a resume reach the two accessors at different
   moments, and each asserts rather than trusts. */
int decide_blob_cursor(const void *blob) {
    const DecideBlob *b = blob;
    long n;

    CHECK(b != NULL,
          "decide: the replay cursor of a flow with no parked decision state was asked for — a running flow's "
          "cursor is live (decide_cursor answers that one) and a parked flow's is its blob, so a NULL here is "
          "a caller reading neither and about to record a flow that has replayed nothing as one at slot zero");
    n = b->seg ? b->seg->below + b->seg->n : 0;
    DCHECK(b->c >= 0 && b->c <= n,
           "a parked flow's decision cursor is outside its own chain — the cursor names the slot the flow "
           "replays next and the chain is every slot it has, so a reading outside it is a distance along a "
           "path this flow does not stand on, and the fraction built from it would exceed one or go negative");
    return b->c;
}

/* See decide.h. FNV-1a over each consumed slot's question and arm, bottom of the chain first; a replay consumes
   the same slots in the same order, so it hashes to the same value. */
void decide_history_name(uint64_t *identity, int *ordinal) {
    const DecSeg *s, **segs = NULL;
    uint64_t h = 1469598103934665603ULL;
    int n_segs = 0, cap = 0, i, k;

    DCHECK(g_running, "a name was minted from a timeline's history while no flow's decision state was loaded");
    DCHECK(identity != NULL && ordinal != NULL, "a history name was asked with nowhere to put it");
    for (s = g_dec_base; s; s = s->base) {
        if (n_segs == cap) {
            cap = cap ? cap * 2 : 16;
            segs = realloc(segs, (size_t)cap * sizeof *segs);
            CHECK(segs != NULL, "decide: OOM walking a flow's decision chain to name what it created");
        }
        segs[n_segs++] = s;
    }
    for (i = n_segs - 1; i >= 0; i--)
        for (k = 0; k < segs[i]->n && segs[i]->below + k < g_c; k++) {
            h = (h ^ segs[i]->k[k]) * 1099511628211ULL;
            h = (h ^ (uint8_t)segs[i]->e[k]) * 1099511628211ULL;
        }
    free(segs);
    for (k = 0; g_dec_below + k < g_c; k++) {
        h = (h ^ g_dec_k[k]) * 1099511628211ULL;
        h = (h ^ (uint8_t)g_dec[k]) * 1099511628211ULL;
    }
    if (g_mint_c != g_c) { g_mint_c = g_c; g_mints = 0; }
    *identity = h;
    *ordinal = ++g_mints;
    CHECK(g_mints > 0, "decide: the per-history mint ordinal wrapped — the next name collides with one already "
                       "minted");
}

void decide_live_stats(long *entries, long *bytes) {
    if (entries) *entries = dec_total();
    if (bytes) *bytes = (long)((size_t)g_dec_cap * DEC_SLOT_BYTES);   /* both head buffers; one capacity governs them */
}

/* The sibling's hot decision state at a fork: the parent's path so far, then `arm` at this branch. c = cursor, so
   on resume the sibling re-executes the OP_if and replays exactly this arm, never re-forking or re-running the
   prefix. `asked` is the branch's own question, which both arms answer. */
static void *decide_fork_blob(int cursor, int arm, uint32_t asked) {
    DecideBlob *b = reclaim_malloc(sizeof *b); CHECK(b, "decide: OOM fork blob");
    /* A fork is always at the end: decide_arm reaches the forking branch only when the cursor has caught up with
       the vector (a replay consumes a recorded slot; a constraint answer records none), so the frozen head ends
       at `cursor`. A fork at an interior cursor would hand the sibling decisions taken after the branch. */
    DCHECK(cursor == dec_total(),
           "a fork was prepared at a cursor that is not the end of the decision vector — the sibling would "
           "inherit decisions taken after the branch it is being forked at");
    b->seg = dec_seg_arm(dec_freeze(), arm, asked);   /* the sibling's arm over the shared prefix; O(1) */
    b->c = cursor;
    dec_blob_take_mints(b);
    return b;
}

/* The other fork's blob (see decide.h). No arm, because no question was asked: the sibling's path is its parent's,
 * and it diverges over a value that arrived, not over a predicate. The freeze is the branch fork's (one shared
 * immutable prefix, O(1)); the cursor is the parent's own, which may be mid-replay.
 *
 * A parent mid-replay re-asks a read its record already answered, and its commitment rows (decide.h) admit only
 * the timeline it took, so no sibling is minted there. Where the rows cannot relate the peer's timelines to the
 * carried ones (the peer's session ended too, flow_answer_fork's residual), this mints siblings holding other
 * answers mid-replay, and they go on to consume arms the parent recorded under its own answer; decide_arm's key
 * assert is what sees that, and it stays unnarrowed. */
void *decide_fork_same_path(const char *why) {
    DecideBlob *b;

    DCHECK(why != NULL && *why,
           "a sibling was minted over a value that arrived with nothing naming what arrived — the census row "
           "this counts into is the frontier's PROVENANCE, and a row with no name merges every mechanism that "
           "forks without a predicate into one number that describes none of them");
    DCHECK(g_running, "a sibling's decision state was forked while no flow's was loaded — the blob would stand "
                      "on whatever chain the previously-switched-in flow left behind, and the sibling would "
                      "replay a path it never took");
    b = reclaim_malloc(sizeof *b); CHECK(b, "decide: OOM forking a flow's decision state");
    b->seg = dec_freeze();   /* the caller's reference, on top of the running flow's own */
    b->c = g_c;
    dec_blob_take_mints(b);
    /* Counted, because this mints a frontier member and @PROGRESS's `forks` is the frontier's growth: the
       provenance `created = 1 + forks + candidates + joined documents + cold resumes` closes only if every
       mechanism that mints one is counted. It gets its own row, named by the caller, not a fabricated predicate key,
       which would report a branch the program never took; each mechanism that forks without a predicate (a peer's
       answer, an orphan drive) is a different row, and the rows still sum to the total. */
    fork_key_count(why, FORK_ROW_SYNTHETIC);
    return b;
}

/* See decide.h. The slot is keyed by `why`, the mechanism's own name, so a replay that reaches a different
   mechanism here diverges (dec_replay's key comparison) rather than consuming an arm recorded for another. */
int decide_completion(const char *why, void **sib_blob) {
    DecideBlob *b;
    uint32_t asked;
    int arm;

    DCHECK(why != NULL && *why == '(',
           "a completion fork was keyed by something other than a mechanism's name — the census files it as a "
           "synthetic row, whose names open on `(`");
    DCHECK(g_running, "a completion fork was asked while no flow's decision state was loaded");
    DCHECK(sib_blob != NULL, "a completion fork was asked with nowhere to hand the sibling's decision state");
    *sib_blob = NULL;
    asked = dec_key_hash(why);
    if (g_c < dec_total()) {
        arm = dec_replay(asked);
        if (arm >= 0) return arm;
        dec_leave_path();
    }
    b = reclaim_malloc(sizeof *b); CHECK(b, "decide: OOM forking a flow's decision state at a completion");
    /* The sibling stands past its own slot: it is assembled already holding its completion and never re-asks
       this question, unlike a branch's sibling, which resumes at the branch and replays its slot there. */
    b->seg = dec_seg_arm(dec_freeze(), 1, asked);
    b->c = g_c + 1;
    dec_blob_take_mints(b);
    fork_key_count(why, FORK_ROW_SYNTHETIC);
    dec_append(0, asked); g_c++;
    *sib_blob = b;
    return 0;
}

/* See decide.h. The freeze and reference are the fork's; the census entry is not, because no frontier member is
   made. A function of its own rather than a flag on the one above, so neither caller reads as the other's special
   case: one mints a flow, one records a path. */
void *decide_freeze_path(void) {
    DecideBlob *b;

    DCHECK(g_running, "a flow's path was frozen while no flow's decision state was loaded — the blob would "
                      "stand on whatever chain the previously-switched-in flow left behind, and every "
                      "candidate seeded from it would replay a path no flow ever took");
    b = reclaim_malloc(sizeof *b); CHECK(b, "decide: OOM freezing a flow's decision path");
    b->seg = dec_freeze();   /* the caller's reference, on top of the running flow's own */
    b->c = g_c;
    dec_blob_take_mints(b);
    return b;
}

/* The predicate's identity, which the flow's constraint is keyed by: the identity of the value the branch tests.
 * A comparison result is a derived value like any other, and concolic.c composes the operator and both operands
 * into its identity, so `if (x)`, `x < 700`, `x < 300`, `700 < x` and `x > 700` are five identities and five
 * independent facts. There is no producer field for a consumer to default, and a merged key would decide one
 * predicate from another and prune an arm the constraint does not contradict, which no gate could see. The
 * composition is sized from its members and cannot truncate, and its length-prefixed fields mean no member can
 * spell another's boundary.
 *
 * Returns a heap key the caller frees, or NULL when the value has no identity this engine can spell (uncertainty,
 * which keeps both arms). It is the branch identity, which differs from the value's own only for a negation:
 * §7.1.2 ToBoolean stands between value and branch in both `if (p)` and `if (!p)`, so they ask one question with
 * the arms swapped, and keying them apart would fork again at `!p` on a flow that has fixed `p`. The caller applies
 * the polarity to the arm, never to this key; concolic_branch_ident_c states it. */
static char *decide_key(JSValueConst cond) {
    const char *f[1];

    f[0] = concolic_branch_ident_c(cond);
    return concolic_ident_compose("branch", f, 1);
}

/* See decide.h. It reads the constraint without touching the decision vector or its cursor: a read is not a
   decision, so it must not consume a slot (a consumed slot would make the next branch read another's answer). */
int decide_value_arm(JSValueConst cond)
{
    char *key = decide_key(cond);
    int arm;

    if (!key) return -1;
    arm = concolic_branch_decided(key);
    free(key);
    /* The key is the predicate's and the answer is this value's; for a negation they differ by the polarity,
       the same XOR decide_branch applies to the arm it returns. -1 is not decided and has no complement. */
    return arm < 0 ? arm : (arm ^ concolic_branch_neg(cond));
}

/* The recorded arm at the cursor, or -1 when the slot there answers a different question, in one descent of the
   chain. The cursor moves only on a match, so a caller that gets -1 stands at the divergence and dec_leave_path
   can end the vector there. */
static int dec_replay(uint32_t asked) {
    uint32_t recorded;
    int arm;

    DCHECK(g_c < dec_total(), "the recorded arm was asked for from past the end of the vector — there is no "
                              "slot there to answer with");
    arm = dec_at(g_c, &recorded) ? 1 : 0;
    if (recorded != asked) return -1;
    g_c++;
    /* Counted here, past the key comparison, because an arm consumed is exactly an arm whose recorded question the
       branch re-asked; above the comparison it would count attempts, which a divergence also is. dec_append
       refuses a slot with no question, so the comparison is an identity comparison for every slot. */
    g_replay_hits++;
    /* The runway rung, observed here because this is the only line that consumes a recorded arm: §@S requires an
       observation site strictly before what a rung measures, and a candidate's replay comes before its sink and
       its source read. The numerator `g_c` is honest only here: the cursor also advances on appends
       (dec_new_decision, dec_nonforking), and past the key comparison it counts exactly the recorded path consumed.
       The denominator is read live; the entry assert `g_c < dec_total()` means no reading is taken after a
       truncation, and the observer is monotone. Only a candidate (a flow with a payload) is asked, which
       flow_observe_replay asserts, as flow_observe_survival does. */
    {
        Flow *f = flow_running();
        if (f && f->cand_payload) flow_observe_replay(f, g_c, dec_total());
    }
    return arm;
}

/* The fork, reached by a branch this flow has never decided, or by one whose recorded answer belongs to another
   question (dec_leave_path having just ended the vector at the cursor). Frame snapshot: prepare the other arm's
   sibling hot state (its decision vector and the current constraint) and signal the interpreter (0x100 bit) to
   clone this frame at the branch; the sibling resumes there and replays its arm, never re-running from the start.
   The observation decides which arm this flow keeps. `real_arm` is §7.1.2 ToBoolean of the example the value
   carries, computed once by the caller and shared with the forced mark, so the primary flow is the one whose
   values a real session would compute (`if (cfg.admin)` over a loaded `false` keeps the false arm), as
   §Learning-from-replies states. The kept arm goes on running and becomes a population while its sibling stays one
   member, so this is a fairness choice too. With nothing observed (-1: external input, absent app state) it keeps
   arm 1, whose meaning the predicate's speller supplies: at the outcome seam the engine numbers its ordinary
   completion 0 and asks it first, so arm 1 ends the machine's walk, while a page's loop test over an unknown length
   reads arm 1 as keep going. Arm 0 would keep the skip arm of `if (__FLAGS.admin)` as the runner, so no constant
   is right for both spellings.
   The sibling's snapshot does not pre-record its arm: it resumes at this branch, re-asks it, takes its recorded
   slot and records the constraint itself. It inherits everything this flow knew before the branch. */
/* `*forked` comes back from the seam meaning "the caller must still snapshot", which differs from "a fork
   happened": a fork always happens here, and whether the caller has work left depends on who can build the
   sibling's resume point (engine_prepare_fork). The parent's own arm is constrained by decide_arm's tail, so
   the constraint is applied in one place for all three ways a decision is reached, and the seam, which may
   assemble the sibling before this returns, sees the parent's constraint as the sibling's frozen copy left it. */
/* Does an arm over `v` against its example take absent application state as present: is every root of `v` a read
   absent.c answered at its injected-state arm? An attacker source's root is never one, so its arms assume nothing. */
static int dec_subject_assumed(JSValueConst v) {
    return concolic_root_every(v, absent_assumed_member);
}

static int dec_fork_here(JSContext *ctx, const char *key, JSValueConst subject, uint32_t asked,
                        int restartable, int real_arm, int *forked, const char *site) {
    int take = real_arm < 0 ? 1 : real_arm;
    /* The sibling's arm is the one the example contradicts, so over absent application state it is an
       assumption, priced on the sibling from its birth at this slot. */
    int assumed_slot = real_arm != REAL_ARM_UNOBSERVED && dec_subject_assumed(subject) ? g_c : -1;
    void *dblob;
    void *pblob;

    DCHECK(real_arm == REAL_ARM_UNOBSERVED || real_arm == 0 || real_arm == 1,
           "a fork was told the real arm is a value that is neither arm nor the unobserved marker — it comes "
           "from one ToBoolean of one example, so a third value is a caller computing it somewhere else");
    /* The census namespaces part here, where the site knows which population it has. A branch over a value
       concolic.c declines to spell (a dynamic key read on an ordinary object, a derivation over one, a comparison
       between two) files a site row named by its shape: where a fork happened, not what it asked, so it is a third
       population rather than a predicate. decide_key answered NULL, so no constraint is recorded; the slot below
       is keyed by the branch's code site instead (dec_site_hash). */
    DCHECK((key == NULL) == dec_key_is_site(asked),
           "a fork is filing into the census under a NAME and into the decision vector under a question hash "
           "that disagree about whether this engine could spell its question — a site row is filed exactly "
           "when no key was spelled, and exactly then the slot is keyed by the branch's code site. Split them "
           "and a predicate's slot could be matched by a code site, or a site's by a predicate");
    if (real_arm == REAL_ARM_UNOBSERVED) g_fork_unobserved++;
    else                                 g_fork_observed++;
    dblob = decide_fork_blob(g_c, !take, asked);
    if (key) fork_key_count(key, FORK_ROW_PREDICATE);
    else     fork_key_count(fork_site_name(subject), FORK_ROW_SITE);
    pblob = concolic_pins_suspend();
    *forked = engine_prepare_fork(ctx, dblob, pblob, key, restartable, site, assumed_slot);
    /* This flow's own arm, onto the head the freeze emptied, under the question both arms answer, so the sibling
       and every later replay of this path check the slot against what they ask and diverge on a mismatch.
       Named residual. Not covered: one code site with the same subject shape met in swapped order across
       occurrences, which a site key cannot tell apart. Next diff builds: a predicate identity for a keyless
       subject from the identity layer. How its absence would show: a resumed flow's emissions after that cursor
       differ from what its family emitted before the park, with `replayLeft` silent. */
    dec_append(take, asked);
    g_c++;
    return take;
}

/* The same new decision in a session that explores nothing: the arm the site declared, recorded like any other,
 * and no sibling. Recording matters as much as the arm: a non-forking flow is still replay(baseline, decision
 * vector), and a browser component's invariants read back what its own ask decided (core/timing/event_loop.h's
 * monotonicity assert reads event_loop_before_decided, because over an unknown a comparison is an arm, not a
 * fact). So it goes through dec_append and decide_arm's one constraint statement; only the member is not minted.
 * Nothing is dropped: the other arm is a world the session declared it is not in. A site with no answer
 * (SOLVER_NO_NONFORKING_ARM) crashes here naming the predicate, because picking an arm for a question the site
 * cannot answer would delete a program at random; the fix is the caller that put an undecidable operand there.
 * It is a CHECK because the value is appended to the decision vector, which the cold tier writes and a later
 * session replays, so a release build would record a byte that is not an arm. */
static int dec_answer_here(const char *key, uint32_t asked, int nonforking) {
    if (nonforking != 0 && nonforking != 1) {
        /* The key's field separators are control bytes (concolic_ident_compose), so the predicate's name is
           copied printable-only for the reader looking for an identifier, as engine.c's C-body fork does. */
        char name[192];
        size_t i;
        for (i = 0; i + 1 < sizeof name && key && key[i]; i++)
            name[i] = (key[i] >= 0x20 && key[i] < 0x7f) ? key[i] : '.';
        name[i] = '\0';
        CHECK_FAILF("a NEW decision was reached in a session that explores nothing, and the site that asked it "
                    "declared no non-forking answer (SOLVER_NO_NONFORKING_ARM) — so there is one world to be in "
                    "and no way to say which. The seam will not pick: a two-armed question its own site cannot "
                    "answer is a program deleted at random. Fix the CALLER that put an undecidable operand in "
                    "front of this site in a non-forking session, not the site. The question was: %s",
                    key ? name : "(no source identity)");
    }
    /* Recorded under the question decide_arm computed, a predicate's or a code site's, as in dec_fork_here. */
    dec_append(nonforking, asked);
    g_c++;
    return nonforking;
}

/* The decision itself, over a predicate identified by `key` (NULL when the tested value has no identity this
   engine can spell: uncertainty, which keeps both arms). Its two callers are the two places control flow turns on
   unknown input: a bytecode branch and a native operation whose completion depends on it. They share the same
   vector slot, constraint and replay; only what the interpreter does with the arm differs.
   A decision is reached one of three ways: refined from the constraint, replayed from a recorded slot (the one
   place a slot is consumed, so its question is compared here), or new. `nonforking` (the site's answer for a
   session that explores nothing; see decide.h) and `real_arm` (what the run observed) are consulted only for a
   new decision, since the other two are answers this flow already has, and re-deciding them from an example
   would be a second evaluator. `subject`, `op` and `completion` are read only to key a branch with no spellable
   question by its code site (dec_site_hash) and name its census row, never for the arm; both entries assert
   `subject` is a concolic (decide_branch's guard, solver_outcome's DCHECK). `op` is NULL for a branch. */
static int decide_arm(JSContext *ctx, const char *key, JSValueConst subject, const char *op, int completion,
                      int restartable, int nonforking, int real_arm, int refined, int *forked, const char *site) {
    uint32_t asked = key ? dec_key_hash(key) : dec_site_hash(ctx, subject, op, completion, site);
    int arm;
    *forked = 0;
    /* The ask, counted above all three arms so a zero `branchRefined` beside a zero `branchAsked` says no
       spellable question was reached. */
    if (key) g_branch_asked++;
    if (key && (arm = concolic_branch_decided(key)) >= 0) {
        g_branch_refined++;   /* the ask above was answered without a slot or a fork; see the declaration */
        /* Feasible refinement, asked first. This flow has already decided this predicate, so the other arm is
           contradicted, and forking again would add a flow that explores nothing. It consumes no slot, because it
           adds no decision. The order (constraint, then recorded arm, then a new fork) is what makes a replay
           reproduce the original run: the vector holds one slot per branch the flow had not already decided, so a
           cold-tier flow replaying its whole path from cursor 0 with an empty constraint consumes exactly the
           recorded slots, and repeat tests of a decided flag cost none. A fork never pre-records the sibling's arm
           into its constraint, so the constraint is never ahead of the cursor. */
    } else if (refined >= 0) {
        /* The constraint's facets leave this predicate one arm (decide_branch's `refined`), so the other is a
           world no value produces, keyed or not. Asked before the vector for the reason above: a replay reaches
           it with the same facets filed and consumes no slot either. */
        arm = refined;
        if (key) g_branch_refined++;
    } else if (g_c < dec_total() && (arm = dec_replay(asked)) >= 0) {   /* REPLAY: a recorded decision */
        /* the cursor moved inside dec_replay, and only because the slot answered this question */
    } else {
        /* A new decision, or one this flow's vector no longer describes; after dec_leave_path those are the same
           state, so there is one fork. A slot answering another question is a divergence, not an abort: the
           recorded tail is left behind and this branch forks like any other. */
        if (g_c < dec_total()) dec_leave_path();
        /* Whether that means a fork is the session's to say, asked here only. Leaving the path happens either way;
           a session that explores mints the sibling, and one that does not takes the arm the site declared. */
        arm = engine_session_forks() ? dec_fork_here(ctx, key, subject, asked, restartable, real_arm, forked,
                                                     site)
                                     : dec_answer_here(key, asked, nonforking);
    }
    /* One place for all three arms: a replayed or refined arm narrows this flow exactly as a forked one does.
       `*forked` says only whether the caller owes a snapshot. It is applied after the fork's freeze, so the
       sibling's frozen constraint does not carry this flow's arm. */
    if (key) concolic_constrain_branch(key, arm);
    DCHECK(g_c <= dec_total(), "the decision cursor ran past the vector — a branch answered without consuming "
                               "its recorded slot, so the next one will read that slot as its own");
    return arm;
}

/* Which arm a session carrying real values would take: the one observation this file makes about a branch, read by
 * dec_fork_here (which arm is the parent's continuation) and by flow.h's `path_forced`. It is computed once, at the
 * top of decide_branch, so the two cannot disagree about one branch.
 *
 * It is an observation, not an invention: where a concolic carries a concrete example (a loaded config field, a
 * modelled viewport's `matches`, an AbortSignal's state, `location.search`'s bytes, or a comparison over any of
 * them, which `concolic_cmp_hook` computes by running §7.2.13 or §7.2.14 on the examples), §7.1.2 ToBoolean of
 * that computed value answers the question. Nothing is solved or re-decided from a stored expression.
 *
 * Absent server state read through a property holds `undefined`, the session's own answer (absent.c), so the
 * arm of `if (window.__FLAGS)` the session does not take contradicts it and is marked forced. A branch with no
 * example answers -1: no observation picked an arm, and neither is marked.
 * `JS_ToBool` runs no page code (§7.1.2 has no ToPrimitive step), so it can be asked inside a branch hook. */
static int decide_real_arm(JSContext *ctx, JSValueConst cond) {
    ConcolicExState st;
    JSValue ex;
    int real;

    /* concolic_example answers JS_UNDEFINED for an absent example too, so presence is the state's: HELD (an
       example, `undefined` included — a member the served record lacks) and DETERMINED (a pin, which may be of
       `undefined`) are bytes, and a falsy one makes the false arm primary. NONE (no bytes) and CONTRADICTED
       (bytes this path disproved) answer unobserved, the sound direction. */
    st = concolic_example_state(cond);
    if (st != CONCOLIC_EX_HELD && st != CONCOLIC_EX_DETERMINED) return REAL_ARM_UNOBSERVED;
    ex = concolic_example(ctx, cond);
    real = JS_ToBool(ctx, ex);
    JS_FreeValue(ctx, ex);
    /* §7.1.2 ToBoolean's only failure is an exception VALUE, which an example can never be: an example rides
       the value the interpreter actually produced, and a throw produces none. */
    DCHECK(real == 0 || real == 1,
           "a concolic value's example coerced to neither true nor false — §7.1.2 ToBoolean answers -1 only "
           "for an exception value, so a producer has attached a pending throw to a value as its example");
    return real;
}

/* The arm this flow ended on, against the same observation: a request that exists only because a gate was forced.
 * It fires for the arm this flow ends on however it was reached (a fork's, a replayed one, a refined one), so a
 * cold-resumed flow keeps its provenance. A sibling marks itself: it re-executes this branch on resume and
 * reaches this line switched in, so no fact is written about one flow while another runs.
 * It makes the two clauses of §Learning-from-replies' sentence together, from one comparison: the forced mark is
 * about the request this flow builds, and dropping the contradicted example is about the values it reads. The
 * drop is keyed by the value's identity and skipped where there is none (filing under anything shared would
 * silence that value in flows that proved nothing); the mark needs no key.
 * It drops the predicate's subject too, where the @H value lives: `x === 'admin'` over an example of `'admin'`
 * taken false would otherwise go on emitting `"/api/" + x` with bytes this path disproved. That is exact for an
 * equality, and for an ordering against a known side: the comparison's example was computed from the operand's,
 * so a contradicted result about operand O contradicts O's example, the inference `concolic_pin` and
 * `concolic_exclude` make. concolic_example asks the pin first under `src_self`, so the holding
 * arm concretizes the value wherever the page holds it and the failing arm degrades it. The subject is named by
 * its identity, while the exclusion uses the hole, because the emission has only printed text. */
/* It is asked of an outcome fork's operand too: a machine's `real` (quickjs.h) is the completion reached when run
 * on the operand's example, so a flow on another completion (`JSON.parse` threw where the example parses, §8.7's
 * timeout negative where the example is 500) contradicts that observation in the same sense. An operand carries no
 * `cmp_subj_ident`, so the second contradiction stays silent; the operand is already named by its own identity. */
/* It answers whether it marked, the one spelling of "this arm contradicts its example" the rest of decide_branch
   reads, so the pin below records `path_pinned` off the same fact rather than re-deriving it. An unobserved
   arm is not marked: its values are computed from inputs and stay `derived`, and only a pin on it is the
   engine's choice. */
static int decide_note_forced_arm(JSValueConst v, int real_arm, int arm) {
    const char *ident;

    DCHECK(arm == 0 || arm == 1,
           "an arm that is neither taken nor not-taken was compared against an example — the contradiction is "
           "between TWO booleans, so a third value here is a decision seam that answered something else");
    if (real_arm == REAL_ARM_UNOBSERVED || arm == real_arm) return 0;
    flow_mark_forced_arm();
    ident = concolic_ident_c(v);
    if (ident) concolic_contradict_example(ident);
    ident = concolic_cmp_subject_ident(v);
    if (ident) concolic_contradict_example(ident);
    return 1;
}

/* The one body behind both branch entries (see decide.h). `restartable` is the caller's declaration about where
   its sibling comes back, the only thing that differs between them. */
static int decide_branch(JSContext *ctx, JSValueConst cond, int restartable, int nonforking,
                         const char *site) {
    const char *src = NULL, *tok = NULL;
    ConcolicLit tok_kind = CONCOLIC_LIT_NONE;
    char *key;
    int op, forked = 0, arm, real, neg, eq_holds, eq_fails, forced_arm, pinned = 0, refined, slot;

    if (!g_running || !concolic_is(cond)) return -1;   /* not a forced-exec branch on a concolic value */

    /* The polarity between the value this branch tests and the predicate it is about, read once and applied
       twice: to the observation on the way in and to the arm on the way out. Between those lines the function
       works in the predicate's terms, so the pin, exclusion, loose-equality domain, bound and call predicate
       need no case for a negation: `arm` means "the page's own gate answered true". */
    neg = concolic_branch_neg(cond);
    DCHECK(neg == 0 || neg == 1,
           "a branch condition carries a polarity that is neither itself nor its complement — the arm below "
           "is XORed with this, so a third value would return an arm the interpreter reads as forked");

    /* If the condition is a comparison result (`x === 'admin'`), the taken arm may pin the source to a concrete
       value (concretize-on-pin), so later reads compute the real @H value. The token's kind comes back with it,
       because §7.1.19 ToString flattens `undefined`, `null`, `0` and `false` onto legal string text: a pin of
       `x === undefined` taken off the spelling would make `x` the truthy nine characters "undefined". */
    op = concolic_cmp(cond, &src, &tok_kind, &tok);

    /* The observation, made once before anything is decided, and read by both decisions that depend on it: which
       arm this flow keeps if the branch is new, and whether the arm it ends on is forced. */
    real = decide_real_arm(ctx, cond);
    /* In the predicate's terms, since `arm` below is. `!p`'s example is the real `!` run on `p`'s, so un-flipping
       reads the same observation from the other end. UNOBSERVED has no complement. */
    if (real != REAL_ARM_UNOBSERVED) real ^= neg;
    key = decide_key(cond);
    /* The arm the compared value's type facet leaves an equality, in the predicate's terms (-1 when both). */
    refined = tok && op != OPCMP_NONE ? concolic_equality_outcome(ctx, cond) : -1;
    slot = g_c;
    arm = decide_arm(ctx, key, cond, NULL, -1, restartable, nonforking, real, refined, &forked, site);
    /* Whether that fork was taken over a subject this flow had already proved (see the counter). Raised here,
       the one line holding both `forked` (decide_arm's answer) and `src` (the subject's source as concolic_cmp
       read it); the pin consulted is from an earlier branch, since this branch's `concolic_pin` is below. */
    if (forked && concolic_src_pinned(src)) g_fork_over_pinned++;

    forced_arm = decide_note_forced_arm(cond, real, arm);
    /* A slot this decision consumed on the arm its example contradicts, over absent application state: the
       sibling replaying its own fork slot, or a resumed flow replaying its path, records the assumption. */
    if (forced_arm && g_c > slot && dec_subject_assumed(cond)) flow_note_assumed_slot(slot);

    /* The source equals tok on the arm that makes the equality true (arm 1; `!==` arrives as `===`). The root
       travels with the pin, read off the comparison result (pred_new gives it the tested operand's root): whose
       bytes this arm demanded, which §Attacker-sources' origin rule needs and no later reader can recover. Which
       equality held travels too: §7.2.13 IsLooselyEqual ( x, y )'s holding arm leaves the operand as either of
       two values (steps 2 and 3), so concolic_pin splits its writes on the algorithm. `eq_holds`/`eq_fails` are
       spelled once for the pin, the exclusion and the loose-equality domain, so the three cannot disagree about
       which arm proved what. */
    eq_holds = op == OPCMP_EQ && arm == 1;
    eq_fails = op == OPCMP_EQ && arm == 0;
    DCHECK(op == OPCMP_NONE ? (!eq_holds && !eq_fails) : (eq_holds != eq_fails),
           "an equality's two arms are neither exclusive nor exhaustive over one branch — a comparison result "
           "that is not an equality must stand on NEITHER (an ordering and a call predicate determine no "
           "value), and one that is must stand on exactly one, or a pin and the exclusion that contradicts it "
           "are recorded off the same run of the same gate");
    /* Every equality arm narrows the compared source's type facet by the algorithm the page wrote
       (concolic_equality_facet); a typeof outcome is a fact about its operand only, so it is never pinned or
       excluded under its own spelling. */
    if (tok && (eq_holds || eq_fails) && concolic_equality_facet(ctx, cond, eq_holds)) {
    }
    else if (src && tok && eq_holds) {
        /* concolic_pin pins `src` only where the compared value is that source; a derivation of it pins nothing. */
        pinned = concolic_pin(cond);
        /* The path records that it has chosen a witness on an arm no example of this run confirmed: one the
           example contradicts, or one over a source with no example (`location.hash`, server state read as a
           bare identifier), where the fork and not an observation chose the arm. From here concolic_new's pin
           arm answers every read of `src` with `tok`, so addresses composed from that source carry bytes this
           engine picked, which the trusted zone's firing decision must tell apart from observed ones. It is
           recorded at the choice because the composed URL is a bare string by then (`pin_mint` answers with a
           primitive and `concolic_add_hook` derives nothing from two); solver/flow.h's field has the full
           reading. Gated on `pinned`: a loose equality or a derived subject determined no value. */
        if (pinned && (forced_arm || real == REAL_ARM_UNOBSERVED)) flow_mark_pinned_value();
    }
    /* The other arm, which is an observation, not an absence: forced execution runs both arms of every
       `x === "admin"`, and the sibling has proved the value is not the token. Without this it would emit its
       parameter with the same bytes as one never tested, which §Solver-half calls a wrong report. It invents
       nothing: `tok` is the side the page wrote and `arm` the arm this run took. A NULL subject (the unnameable
       `{}`) has no hole the @H surface prints. It is sound under both equalities: §7.2.14 IsStrictlyEqual
       ( x, y ) step 1 makes `x === tok` imply SameType, and §7.2.13 IsLooselyEqual ( x, y ) step 1 then returns
       the same answer, so `x === tok` implies `x == tok`, and a failed loose equality proves the operand is not
       the token as strictly as a failed strict one. */
    else if (tok && eq_fails) {
        const char *subj = concolic_cmp_subject(cond);
        if (subj) concolic_exclude(subj, tok);
    }
    /* The holding arm of a loose equality: concolic_pin refuses a value here (§7.2.13's holding arm leaves a set,
       and writing a member would pick a witness), but the observation is still recorded as a domain, so both arms
       of one `==` gate reach the report. It is a sibling of the branches above, not an `else`, because it branches
       on the algorithm the page wrote rather than the arm. It is keyed by the hole, as a domain is, not by `src`
       (concolic_cmp_subject states the pair). `concolic_cmp_algo` aborts on a non-equality, so `eq_holds`, false
       for every OPCMP_NONE, is evaluated first. No value is determined by a predicate answering true. */
    if (tok && eq_holds && concolic_cmp_algo(cond) == JS_CONCOLIC_EQ_LOOSE && !concolic_cmp_is_typeof(cond)) {
        const char *subj = concolic_cmp_subject(cond);
        DCHECK(tok_kind != CONCOLIC_LIT_NONE,
               "an equality reached the loose-domain record with an operand this engine cannot spell — "
               "pred_new asserts that a token and its kind are written together, so a spelling with no kind "
               "would file `x == undefined` and `x == \"undefined\"` under one row, and §7.2.13 steps 2 and 3 "
               "give those two gates holding sets with nothing in common");
        if (subj) concolic_looseeq(subj, tok_kind, tok);
    }
    /* The ordering: the same observation over an ordered domain, with a bound on each arm and no value on
       either. A sibling, not an `else`: a comparison result is an equality or an ordering, never both, which
       concolic_rel asserts. The false arm's relation is §13.10.1's paired production (rel_op_negate states what
       the pairing does with §7.2.12's `undefined`), so `x < 5` taken false records `x >= 5`. Nothing picks a
       member of the interval: the emission stays a domain-annotated shape. */
    {
        const char *rtok = NULL, *rsubj = NULL;
        double rnum = 0;
        RelOp rel = concolic_rel(cond, &rtok, &rnum, &rsubj);
        if (rel != REL_NONE) {
            DCHECK(op == OPCMP_NONE,
                   "one comparison result answered as an equality AND as an ordering — the two are minted by "
                   "different hooks over different operands, so a value carrying both is one predicate whose "
                   "two records would state two different domains for one hole");
            DCHECK(arm == 0 || arm == 1,
                   "a bound was about to be recorded for an arm that is neither taken nor not-taken — the "
                   "relation a flow stands on is decided by WHICH arm it took, so an undecided one would "
                   "file the true arm's constraint under a path that may run the false one");
            concolic_bound(rsubj, arm ? rel : rel_op_negate(rel), rnum, rtok);
            concolic_ordering_facet(cond, arm);
        }
    }
    /* The third class: a method call over an unknown whose result is the boolean tested, §@H's headline shape
       (`{startsWith:/api}`). A sibling, not an `else`, for the ordering's reason. The arm is the fact, carried
       rather than folded into the predicate: a method has no paired method, and inventing one would decide what the
       method means. The method is the name the page wrote and the arguments the literals it passed. */
    {
        ConcolicCallPred sp;
        if (concolic_strpred(cond, &sp)) {
            DCHECK(op == OPCMP_NONE,
                   "one condition answered as an equality AND as a call predicate — an equality result is "
                   "minted by the comparison hook and a call result by the call handler, so a value carrying "
                   "both is one predicate whose two records would state two different domains for one hole");
            DCHECK(concolic_rel(cond, NULL, NULL, NULL) == REL_NONE,
                   "one condition answered as an ordering AND as a call predicate — the same two-records "
                   "defect the equality assert above names, reached from the other side");
            DCHECK(arm == 0 || arm == 1,
                   "a call predicate was about to be recorded for an arm that is neither taken nor "
                   "not-taken — the arm IS the fact here, so an undecided one would file `startsWith` held "
                   "under a path that may have proved it did not");
            concolic_strpred_file(sp.subject, sp.method, sp.args, sp.nargs, arm);
        }
    }
    /* Named residual: a bare truthiness gate narrows and is recorded under a key the @H surface cannot look up.
       Not covered: a condition that is the unknown itself (`if (cfg.admin)`), for which concolic_cmp,
       concolic_rel and concolic_strpred all answer nothing, though §7.1.2 "ToBoolean ( arg )" narrows it. The
       observation is kept: decide_arm files it through concolic_constrain_branch under decide_key's `branch`
       identity, so N tests of one flag cost no forks; but `kv_add`'s four reads are keyed `excl`, `bound`,
       `strpred` and `looseeq` over the hole, and none spells it.
       Next diff builds: a fifth recorder keyed by `concolic_hole_key(concolic_shape_c(cond))`, filed on both arms
       (ToBoolean determines no value on either), read at `kv_add` under the same hole, merged in
       extension/lib/learn.js by intersection as `intersectLooselyEquals` is, with its name added to
       `AST_PARAM_KEYS` in the same diff; it states the arm, never a member of the false arm's set.
       How its absence would show: a param with `valueClass` "unknown" and all four domain keys absent, on a path
       whose flow forked at the gate (witnessed by the forked arm's own endpoint) and was later refined by it. */
    /* What this flow assumed here, when no observation established the arm (its example contradicts it, or
       there is none) and the arm determined no value: the pin above states an arm that did, and an endpoint's
       witness reads both (solver/endpoint.c). */
    if ((real == REAL_ARM_UNOBSERVED || arm != real) && !pinned && key) concolic_note_assumed_branch(key, concolic_shape_c(cond), arm, neg);
    free(key);
    /* Back into the value's terms: everything above recorded facts about the predicate, and the interpreter asked
       about the condition, which for a negation is the complement. The forked bit is about the branch, not its
       reading. */
    arm ^= neg;
    return forked ? (arm | SOLVER_FORKED_BIT) : arm;   /* the bit tells the interpreter to snapshot-fork this frame */
}

int solver_decide_at(JSContext *ctx, JSValueConst cond, int nonforking, const char *site) {
    DCHECK(site != NULL, "a branch decision was asked for with no ask site — the site is captured at the "
                         "CALLER by SOLVER_SITE_HERE and threaded by every intermediate, so a NULL here is "
                         "a chain that dropped it and an abort that cannot name what to convert");
    return decide_branch(ctx, cond, 0, nonforking, site);
}

int solver_decide_restartable_at(JSContext *ctx, JSValueConst cond, int nonforking, const char *site) {
    int r = decide_branch(ctx, cond, 1, nonforking, site);
    DCHECK(r < 0 || !SOLVER_FORKED(r),
           "a restartable branch was told to snapshot a frame — the seam assembles this sibling itself "
           "(there is no activation to clone), so the bit can only mean the seam took the other path and the "
           "caller is about to strand a prepared blob");
    return r;
}

/* JSFlowControlHooks.outcome: the same decision asked from a C builtin, which has no OP_if. `JSON.parse(x)` over
 * unknown text completes with a value or a SyntaxError, both feasible, and picking one would delete the arm a
 * `catch` lives on; the machine states the question and this answers it from the decision vector, so a parked
 * sibling resumed next session takes the same arm. The key composes the operand's identity, the operation and the
 * completion through the engine's one encoding, so `x === "a"` and `JSON.parse(x)` are two facts about one value;
 * a machine asking at successive positions (an iteration over an unknown collection) puts the position in `op`.
 *
 * `real` is the machine's own statement of which completion a session carrying real values reaches, a parameter
 * for `nonforking`'s reason: §7.1.2 ToBoolean of the operand's example answers a different question, and this
 * seam holds the operation's name, never its semantics, so deciding from the name would be a recognizer.
 * JS_OUTCOME_REAL_UNSTATED is the machine's "I cannot say", mapped onto REAL_ARM_UNOBSERVED. A stated `real` is
 * read twice: by decide_arm (a new decision keeps the real completion) and by decide_note_forced_arm (the
 * completion this flow ends on, however reached, is compared against it, so a contradicted flow marks itself
 * forced). */
/* N completions are N-1 binary decisions, so there is no N-way slot and no sibling queue. engine_prepare_fork's
 * handoff holds one sibling and the step driver takes one JS_FlowClone per JS_STEP_FORK, so each ask asks "is it
 * c?" for successive completions: the parent answers yes at the first question it reaches as new and returns; the
 * sibling carries the no, resumes at the ask (fork_phase still FORK_PH_ASK), replays its no, reaches the next
 * question as new and forks once itself. N flows, N-1 forks, drawn lazily one link per time the scheduler picks a
 * sibling. Each sibling is an ordinary fork from a parent standing at its own branch (rank-neutral); the chain's
 * state is the decision vector, which parks and replays; N is uncapped; and the questions partition [0, n).
 * The order asks about `real` first, because the parent must answer yes at its first new question and
 * §Learning-from-replies makes the real arm primary; the rest follow ascending. `real` is dropped after the first
 * question, since a flow past it has contradicted the example, and an unobserved real makes dec_fork_here take arm
 * 1, so a new question always ends the walk. With `real` unstated, completion 0 is asked first, matching the
 * non-forking driver's `harm = 0`, so an @S re-fire walks the primary's completion.
 * The walk answers N questions while asking N-1, so it settles at its exit (outcome_settle): which completions get
 * slots depends on `real`, so one fact could otherwise sit under two keys (at n == 2, "is it 1" and "is it 0"), and
 * the sibling this seam mints, whose example it contradicts, would compose the other spelling and re-fork it. */
/* The elimination order: the i-th completion asked about, as a permutation of [0, n). A function, not an array,
 * because it is read on every ask of every replaying sibling and has nothing to own; its caller asserts it is a
 * bijection. With `real` unstated it is the identity permutation. */
static int outcome_nth(int i, int n, int real) {
    int c;

    DCHECK(i >= 0 && i < n, "the elimination order was asked for a position outside the completions the "
                            "machine declared — a position past the end names no completion, and the walk "
                            "that reads it would answer with one the machine has no algorithm for");
    if (real == JS_OUTCOME_REAL_UNSTATED) c = i;
    else if (i == 0) c = real;
    else c = (i - 1 < real) ? (i - 1) : i;   /* ascending over the others: shift past the hole `real` left */
    /* The order's range, asserted at its origin: the value becomes the seam's return and the step driver's
       `h->fork_arm`, where an undeclared completion is an arm the machine has no algorithm for. */
    DCHECK(c >= 0 && c < n,
           "the elimination order named a completion outside the ones the machine declared — the walk would "
           "hand the step driver an arm no branch of that machine answers");
    return c;
}

/* The one speller of an outcome question's key, "is this operation's completion over this operand exactly c", used
 * by the walk and by the settle, which must file facts under the names the walk looks up. Returns a heap key the
 * caller frees, or NULL for an operand whose identity this engine cannot spell. */
static char *outcome_key(JSValueConst over, const char *op, int c) {
    /* Sized from the bound on `c`: solver_outcome asserts `n <= SOLVER_FORKED_BIT` (0x100) and outcome_nth
       asserts `c < n`, so the widest value is `255`. A truncated completion would file two completions under one
       key; the DCHECKF fails if the bound and this size stop agreeing. */
    char which[4];
    const char *f[3];
    int wrote = snprintf(which, sizeof which, "%d", c);

    DCHECKF(wrote > 0 && (size_t)wrote < sizeof which,
            "the outcome seam could not spell the completion its question is about (%d) — `which` is sized "
            "from SOLVER_FORKED_BIT, which bounds every completion a machine may declare, so a completion that "
            "does not fit means that bound and this buffer have stopped agreeing", c);
    f[0] = concolic_ident_c(over); f[1] = op; f[2] = which;
    return concolic_ident_compose("outcome", f, 3);
}

/* The walk's answer, written down for every question it answered. `c` is the completion the walk ended on, and
 * the questions partition [0, n), so "the completion is c" settles "is it d" for every d: entailment, not
 * observation, asking nothing, consuming no slot and forking nothing. It runs after the fork, and dec_fork_here
 * freezes the sibling's constraint before decide_arm applies the parent's arm, so these entries land in the
 * parent alone; the sibling settles its own when it re-walks. The assert reads before it writes: a recorded truth
 * that disagrees with the returned completion is a flow standing in a world its own path denies, and overwriting
 * it would hide that. */
static void outcome_settle(JSValueConst over, const char *op, int n, int c) {
    int d;

    DCHECK(c >= 0 && c < n, "the outcome walk settled on a completion the machine never declared");
    for (d = 0; d < n; d++) {
        char *key = outcome_key(over, op, d);
        int want = (d == c), had;

        /* No key means nothing is recorded, the settle's analogue of the walk keeping both arms for an operand
           with no identity. A `return`, because the operand is the same at every d. */
        if (!key) return;
        had = concolic_branch_decided(key);
        DCHECKF(had < 0 || had == want,
                "an outcome walk ended on completion %d over an operand whose own path had already answered "
                "\"is it %d\" the other way — the questions partition the completions, so one flow holding "
                "both is standing in a world no completion satisfies, and the operation it is about is `%s`. "
                "The cause is a question filed under two keys: the walk asks about `real` first, so the "
                "completion in the key is the machine's EXAMPLE, and this seam's own decide_note_forced_arm "
                "contradicts that example on the sibling it mints — after which the same fact composes the "
                "other spelling", c, d, op);
        concolic_constrain_branch(key, want);
        free(key);
    }
}

/* The completion this walk ended on, filed as an assumed decision under its settled key when it is not the
   completion the machine declared real (or the machine declared none). An operand with no identity has no
   key, as outcome_settle answers for it. */
static void outcome_note_assumed(JSValueConst over, const char *op, int c) {
    char *key = outcome_key(over, op, c);

    if (!key) return;
    concolic_note_assumed_outcome(key, op, concolic_shape_c(over), c);
    free(key);
}

int solver_outcome_at(JSContext *ctx, JSValueConst over, const char *op, int n, int real,
                      const char *site) {
    int i, forked = 0, slot;

    if (!g_running) return -1;
    /* The operation and the site go in the message, so a reader can tell which of the interpreter's callers
       asked. `op` is the operation string the caller composes with its cursor in it; `site` is the asking
       opcode's `__FILE__`/`__LINE__`, which begins at quickjs.h's `JS_FLOW_SITE_HERE` and is forwarded by every
       hop. The two readings take opposite work: the caller's entry condition admits an operand with one
       completion, or a pin determined the operand between the chain's decision to ask and this call. `op` may be
       NULL here, since its own assert is further down. */
    DCHECKF(concolic_is(over),
            "the outcome seam was asked about a value that is not unknown — a native operation forks only "
            "where its operand's domain permits more than one completion. The operation is `%s` and the ASK "
            "is at %s, which is the opcode's own site and not this seam's. TWO READINGS, and they take "
            "opposite work: either that caller's entry condition admits an operand with one completion, which "
            "is a guard owed where the chain decides to ask, or a pin DETERMINED the operand between the "
            "decision and this call, which is a fact about that chain's sequence instead",
            op ? op : "(the caller named none, which the assert below reports)",
            site ? site : "(no site was threaded)");
    DCHECK(n >= 2, "an outcome fork declaring fewer than two feasible completions — one completion is not a "
                   "fork, it is the answer, and a machine that reached this seam with one has handed the flow "
                   "a decision it had already made itself");
    /* The only limit on N, the return protocol's: the arm crosses back in the low byte of one int and
       SOLVER_FORKED_BIT owns the next, so a larger completion would arrive at `harm &= 0xff` as another
       completion with the fork bit set. Widen SOLVER_FORKED_BIT and the driver's mask together to move it. */
    DCHECK(n <= SOLVER_FORKED_BIT,
           "an outcome fork declared more completions than the seam's return value can name — the arm crosses "
           "back to the step driver in the bits below SOLVER_FORKED_BIT, so a completion at or above it would "
           "be delivered as another completion carrying the fork bit");
    /* The shape of the machine's declaration, asserted at the consumer: a value that merely looks like an arm
       would order the walk and be compared against by decide_note_forced_arm. */
    DCHECK(real == JS_OUTCOME_REAL_UNSTATED || (real >= 0 && real < n),
           "an outcome fork's REAL completion is neither one of the completions the machine declared nor the "
           "positive 'this machine cannot say' — the seam cannot compute this one, so a third value is a site "
           "spelling its declaration wrong rather than a fact this file can repair");
    DCHECK(concolic_src_c(over) != NULL,
           "an unknown operand with no source identity reached the outcome seam — its completions cannot "
           "be constrained, so two asks about it would be one fact");
    DCHECK(op != NULL,
           "a native operation asked the outcome seam to decide a completion without naming ITSELF — the "
           "operation is half the predicate, and two operations over one operand would be one fact");
    for (i = 0; i < n - 1; i++) {
        int c = outcome_nth(i, n, real);
        /* The machine's declaration is read at the question it is about: at i == 0 the question is "is the
           completion `real`", so the observed arm is 1; past it, this flow has recorded that it is not. */
        int real_arm = (i == 0 && real != JS_OUTCOME_REAL_UNSTATED) ? 1 : REAL_ARM_UNOBSERVED;
        char *key;
        int arm;

        /* The order is a permutation: `real` is asked at position 0 and skipped after, so meeting it again would
           ask one completion twice and another never. */
        DCHECK(i == 0 || real == JS_OUTCOME_REAL_UNSTATED || c != real,
               "the elimination order asked about the machine's REAL completion a second time — the order must "
               "be a permutation of the completions, and a repeat leaves one of them with no question and one "
               "flow standing on a completion nothing eliminated");
        key = outcome_key(over, op, c);
        /* A NULL key is uncertainty, decided exactly as decide_branch decides it. An absent identity bottoms out
           at an Object or Symbol (`operand_kind` classifies it CONCOLIC_LIT_NONE, whose identity would be its
           address, and concolic_ident_compose propagates absence), which literal_ident and
           concolic_key_read_hook declare correct. It costs forks, never the arm: both completions stay, no
           constraint or slot is recorded, and dec_fork_here files the sibling under FORK_ROW_SITE.
           Named residual. Not covered: a second ask about the same operand and operation re-forks rather than
           being refined; a replay re-reaching this completion takes its code-site slot. Next diff builds: a
           spellable identity for an ordinary object from program facts (JS_CreationName pairs a site with the
           creating flow's mint ordinal, but its site is a bytecode body locator, which an ordinary object lacks).
           How its absence would show: a large FORK_ROW_SITE row in `_forkAt` with no predicate row beside it. */
        /* Not restartable: an outcome ask comes from a C body mid-algorithm, so the sibling's resume point can only
           be the step driver's snapshot of the machine. No non-forking arm: a session that does not fork installs
           no outcome hook, and the driver takes outcome 0 itself (core/timing/timer.c states the convention at
           §8.7's step 4), so SOLVER_NO_NONFORKING_ARM makes reaching here in such a session crash naming the
           question. */
        slot = g_c;
        arm = decide_arm(ctx, key, over, op, c, 0, SOLVER_NO_NONFORKING_ARM, real_arm, -1, &forked, site);
        free(key);
        /* The arm this flow ends on, against the machine's declaration, as decide_branch does, before the forked
           bit (a message to the driver, not part of the arm) is composed. Only i == 0 can fire it. */
        if (decide_note_forced_arm(over, real_arm, arm) && g_c > slot && dec_subject_assumed(over))
            flow_note_assumed_slot(slot);
        if (arm == 1) {
            /* The answer, written under every name it has (see outcome_settle), after decide_note_forced_arm,
               which may have taken this operand's example away, and on the arm that ended the walk, which also
               proved "not d" for every completion it never reached. */
            outcome_settle(over, op, n, c);
            if (c != real) outcome_note_assumed(over, op, c);
            return forked ? (c | SOLVER_FORKED_BIT) : c;
        }
        /* One fork per ask: dec_fork_here keeps arm 1 for both values this walk passes (1 and UNOBSERVED), so a
           new question always returns above. A sibling prepared here would precede a second prepare into
           engine_prepare_fork's one-occupant slot, dropping the first sibling's blobs. */
        DCHECK(!forked,
               "an outcome question that ELIMINATED a completion still minted a sibling — the walk is about to "
               "ask a second new question inside one ask, and the step driver takes exactly ONE clone per "
               "JS_STEP_FORK, so the sibling prepared here has no frame coming and the next prepare overwrites "
               "its blobs");
    }
    /* The last candidate needs no question, since N-1 answers determine it, so the fall-through mints nothing and
       claims no slot; `forked` is 0 here by the assert above. It still needs a record: the settle writes the
       completion this walk proved, so a later ask that composes this completion's key finds it. */
    {
        int last = outcome_nth(n - 1, n, real);
        outcome_settle(over, op, n, last);
        if (last != real) outcome_note_assumed(over, op, last);
        return last;
    }
}
