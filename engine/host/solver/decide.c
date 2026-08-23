/* The frame-agnostic decision — see decide.h. */
#include "solver/decide.h"
#include "solver/concolic.h"
#include "solver/flow.h"
#include "solver/engine.h"
#include "solver/reclaim.h"   /* the engine's own allocations ask for a flow back before they fail */
#include "check.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>

static void *decide_fork_blob(int cursor, int arm, uint32_t asked);   /* the sibling's hot state (below) */

/* THE DECISION VECTOR IS A CHAIN, NOT A COPY — the fourth and last instance of cow.c's refcounted immutable
 * segment, and the one that was still copying.
 *
 * A FORK SHARES THE PREFIX. The sibling's decision vector is, by construction, the parent's decisions up to the
 * branch plus one byte: the other arm. That prefix is FROZEN at that instant — neither flow can ever rewrite a
 * decision it has already taken, because the cursor only moves forward and a replayed slot is read, never
 * written — so it is exactly the shape cow.c's CowSeg, dom_cow.c's DomSeg and concolic.c's ConsSeg already
 * have, and it was the only one of the four still handing each sibling its own `malloc(cursor)` + `memcpy`.
 *
 * MEASURED, on the minimal fixture, whose unknown-length walk forks once per position so the chain is as long
 * as the walk (@COLD in the progress stream, which is the census this file's cost had no row in before):
 *
 *     flows       1006      4014      9006     16046
 *     decKiB       506      7916     39720    125915     <- copied: the whole prefix, per flow
 *     decKiB         7        31        70       125     <- chained: one blob header per flow
 *     decSegKiB     41       164       369       658     <- …plus the shared chain, counted ONCE
 *     cLiveKiB    4057     18559     62226    165039     <- the whole engine's live C allocation, copied
 *     cLiveKiB    3633     10974     23249     40447     <- …chained
 *
 * 125915 KiB against 125+658 — and the ratio is not the point, the SHAPE is: the first row is quadratic and
 * the other two are linear, so the gap is unbounded. Every one of those numbers is a matched FLOW COUNT and
 * not a matched wall clock, which is the only honest comparison here: the copy was O(n) work per fork as well
 * as O(n) bytes, so the same wall clock now reaches 29550 flows where it reached 16046.
 *
 * That is n^2/2 bytes to the byte — 16046 flows held 128,745,080 decision slots, and 16046^2/2 is 128,736,529 —
 * and it was 123 MB of the 137 MB of per-flow snapshot the frontier was holding, against 15 MB for every
 * suspended heap-frame chain in the run put together. A quadratic term in the thing a cold tier exists to page
 * is not a paging problem, it is the wrong data structure: paging it out would have written the same shared
 * prefix once per flow and multiplied the sharing back out, which is precisely what §State-isolation forbids
 * ("The delta captures ONLY shared baseline state").
 *
 * WHAT IS PER-FLOW IS THE HEAD: the decisions this flow has taken since its last freeze, which for a flow that
 * forks at every branch is exactly one. Everything below it is shared with its ancestors, refcounted, and freed
 * when the last flow standing on it goes.
 *
 * `g_c` is the cursor in WHOLE-VECTOR coordinates, so `below` on each segment is the index its first slot has —
 * stored rather than re-derived, for the reason TrampFrame's `local_slots` is: a length that is computed twice
 * can disagree with itself. */
/* `park_id` IS THE SEGMENT'S NAME IN THE PARK DOCUMENT, and it lives HERE rather than in a pointer table the
   pager keeps, because a pointer is not an identity across a FREE. The cold tier writes each frozen segment
   once and names it by a dense ordinal; a park that runs several times in one session — which is what a PARTIAL
   self-park is — releases the flows it wrote between two of those runs, so a segment nothing stands on any more
   is freed and the allocator may hand the SAME ADDRESS back for a segment on a completely different path. A
   table keyed by address would then answer the second segment with the first one's ordinal, and every flow
   standing on it would resume onto a path it never took, silently. A name that dies with the thing it names
   cannot do that: -1 until the pager assigns one, and every construction site below sets it. */
/* `k` IS WHICH QUESTION EACH ARM ANSWERS, and without it a decision vector is a sequence of answers with no
   record of what was asked — which is only safe while the replay asks the identical questions in the identical
   order, and that is precisely what a resume cannot promise. A cold-resumed flow starts at cursor 0 and re-runs
   the document against TODAY's code and TODAY's replies (decide_blob_new says so, and §Time-travel-resume
   requires it: "a resumed flow re-derives example VALUES from CURRENT sources"). The moment one question
   appears, disappears or moves, the cursor is off by one and decide_arm's replay branch hands every subsequent
   branch the arm belonging to some other predicate — a real arm, in range, satisfying every assert on the path,
   all of which are RANGE asserts. That is the silent failure, and it is silent in both directions: a clean
   resume and a scrambled one emit byte-identical output, so no gate this project has could tell them apart.
   The column is the identity that makes the two distinguishable. It is a CONTENT HASH of the constraint key,
   for the same reason JSStepHdr::fork_ask_key is one ("The bytes are the only thing that is the same question
   in every tier") — the key string is heap-allocated per ask and freed immediately, a rebuilt segment has no
   address at all, and the park document carries text. A hash can only ever fail to CATCH a mismatch, never
   invent one, which is the property an assert standing over every replayed branch has to have.
   ONE ALLOCATION HOLDS BOTH COLUMNS: `k` first (so malloc's own alignment is the uint32 array's) and `e`
   immediately after it, because a second reclaim_malloc per segment is a second allocation per FORK, and this
   file's whole history is about what per-fork cost does at 16046 flows. `e` is not separately owned and is
   never separately freed — dec_seg_unref frees `s->k` and that is the whole block. */
typedef struct DecSeg {
    uint32_t *k;        /* the asked question per slot; OWNS the block, with `e` inside it */
    signed char *e;     /* the arms — points into `k`'s allocation at offset n*sizeof(uint32_t) */
    int n, below; struct DecSeg *base; int refcount; long park_id;
} DecSeg;

/* WHAT ONE SLOT COSTS in the frozen chain and in the head — stated once, because the census subtracts exactly
   what the allocation added and a size written twice can disagree with itself. */
#define DEC_SLOT_BYTES (sizeof(uint32_t) + 1u)

static DecSeg *g_dec_base = NULL;   /* the frozen prefix the running flow stands on (NULL = it has none) */
static int g_dec_below = 0;         /* slots in that chain — the head's index origin */
/* THE HEAD IS TWO PARALLEL BUFFERS AND NOT ONE BLOCK, which is the opposite of the frozen segment's layout
   above and is the right answer for the opposite reason: the head is ONE reusable buffer for the whole session
   (decide_free owns it), so its allocation count is a constant and its two reallocs cost nothing per fork —
   while splitting a growing block would have to re-place `e` inside it on every growth. */
static signed char *g_dec = NULL; static int g_dec_n = 0, g_dec_cap = 0;   /* the head: this flow's own */
static uint32_t *g_dec_k = NULL;   /* …and which question each of its slots answers */
static int g_c = 0;
static JSValue g_cur_fn = JS_UNDEFINED;   /* borrowed from the running Flow (alive for the run) */
static int g_running = 0;

/* THE FORK CENSUS'S ROWS — declared here rather than beside fork_key_count because decide_free releases them
   and stands above it, and a census whose teardown could not see it would hold one strdup per distinct
   predicate past the session that made them. See fork_key_count for what the table is. */
#define DECIDE_FORK_KEYS 64
static struct { char *key; long n; } g_fork_keys[DECIDE_FORK_KEYS];
static long g_fork_other, g_fork_total;

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

/* THE ASKED QUESTION'S IDENTITY, as 32-bit FNV-1a over the constraint key's bytes.
 *
 * IT IS THE ENGINE'S OWN PRECEDENT AND NOT A CHOICE MADE HERE: quickjs.c's step_fork_key hashes an operation
 * string exactly this way, for exactly this hazard one tier down — "an outcome fork's answer was delivered to a
 * DIFFERENT question than the one that asked for it" — and states the property that makes it the right shape
 * for an assert ("A hash can only ever fail to CATCH a mismatch, never invent one"). That mechanism cannot be
 * reused here because it lives on JSStepHdr, which dies with the frame chain and therefore cannot cross a park;
 * this column is the same fact recorded on the one part of a snapshot that does cross.
 *
 * 32 BITS, AND THE WIDTH IS ARGUED RATHER THAN INHERITED. The likeliest divergence is a SINGLE inserted or
 * deleted question, after which every remaining slot is misaligned by one — so the check gets many chances,
 * but the DAMAGE is done at the first misaligned slot and a miss there is the exact silence this column
 * exists to end. An 8-bit key false-agrees at that slot 1 in 256, which for the product's central claim is a
 * lottery rather than a check; it saves 3 bytes per slot, and this file's own measurements put the whole frozen
 * chain at 658 KiB across 16046 flows, so the saving is tens of kilobytes against a 1-in-256 chance of passing
 * the one event the column is for. A hash the engine already spells at a width it already argued is also the
 * only way there is ONE answer to "how is a question named" rather than two that drift.
 *
 * ZERO IS RESERVED FOR A QUESTION THIS ENGINE CANNOT SPELL — decide_key returns NULL for a value with no
 * identity (uncertainty, which keeps both arms), and every such branch hashes to 0 and therefore AGREES with
 * every other such branch. That is not a hole the check should close: two branches the engine cannot tell
 * apart are two branches it cannot tell apart, and inventing a distinguishing name for them would be a key
 * that claims knowledge the identity layer does not have. The limit is real, it is stated here, and it is
 * narrowed by making concolic.c able to spell more identities — never by widening this hash. */
static uint32_t dec_key_hash(const char *key) {
    uint32_t h = 2166136261u;

    if (key == NULL)
        return 0;   /* "no identity this engine can spell" — see above; a zeroed slot already reads as one */
    while (*key) {
        h ^= (unsigned char)*key++;
        h *= 16777619u;
    }
    return h ? h : 1u;   /* 0 is reserved, so a key that hashes to it is nudged off the reservation */
}

/* THE FROZEN CHAIN'S OWN CENSUS — the fourth of the four, counted at the two points a segment's lifetime begins
   and ends so the pair cannot drift from the thing it counts. */
static long g_dec_seg_live, g_dec_seg_entries_live, g_dec_seg_bytes_live;

/* Drop a chain reference: refcount--, free the segment at zero, continue into its base. A loop, not recursion —
   the chain's depth is the fork depth, and an unknown-length walk makes that as deep as the walk is long; the C
   stack cannot be parked. The twin of cow_seg_unref / dom_seg_unref / cons_seg_unref. */
static void dec_seg_unref(DecSeg *s) {
    while (s && --s->refcount <= 0) {
        DecSeg *base = s->base;
        /* THE CENSUS'S OTHER HALF. Its own comment says the pair is "counted at the two points a segment's
           lifetime begins and ends so the pair cannot drift from the thing it counts" — and only the BEGIN
           point was written, so the count could only ever rise. `decide_free`'s teardown DCHECK then fired on
           every run that froze a single segment, reporting a leak of memory these two `free`s had already
           released. A counter that cannot go down is not a census of what is live; it is a total, and asserting
           on it says nothing about the heap. */
        g_dec_seg_live--; g_dec_seg_entries_live -= s->n;
        g_dec_seg_bytes_live -= (long)sizeof *s + (long)((size_t)s->n * DEC_SLOT_BYTES);
        /* ONE FREE FOR THE ONE BLOCK. `e` points INSIDE `k`'s allocation (see the struct), so freeing it here
           would free a pointer the allocator never handed out — the two columns are one owned thing and this
           is the one site that says so. */
        free(s->k); free(s);
        s = base;
    }
}

void decide_chain_stats(long *segs, long *entries, long *bytes) {
    if (segs) *segs = g_dec_seg_live;
    if (entries) *entries = g_dec_seg_entries_live;
    if (bytes) *bytes = g_dec_seg_bytes_live;
}

/* The running flow's decision state is over: release its chain and empty its head. `g_dec` itself is the ONE
   reusable head buffer and is not freed here — decide_free owns it. */
static void dec_clear(void) {
    dec_seg_unref(g_dec_base);
    g_dec_base = NULL; g_dec_below = 0; g_dec_n = 0;
}

static int dec_total(void) { return g_dec_below + g_dec_n; }

/* The arm recorded at whole-vector index `k`. The head first, because that is where a SIBLING's read lands: it
   resumes with its cursor AT the branch it was forked over, so the one slot it replays is the topmost.
   THE COLD TIER'S RESUMED FLOW READS THE OTHER END, and that is worth saying because the walk below is O(chain)
   per read for it: it starts at cursor 0, so its first read is the BOTTOM segment and the loop descends the
   whole chain to reach it, then the next read descends all but one, and so on — O(depth^2) to replay a path of
   depth D, with D forks-deep chains made of one-arm segments. That is a few tens of milliseconds at the depths
   this engine has measured (8000) and it is not a bound (nothing is dropped or truncated), but it is the cost
   that grows fastest in a resume and it has one obvious root fix: a resume walks its arms in ASCENDING order,
   so the chain can be indexed ONCE at decide_resume into the flow that is running — one array for the one
   switched-in flow, never one per parked flow, which is the distinction that made the flat vector the wrong
   shape to park in the first place. */
/* …AND IT ANSWERS BOTH COLUMNS IN ONE WALK, through `asked`. The arm and the question that produced it are one
   fact about one slot, and reading them with two descents of the chain would be two walks that can disagree
   about which slot they landed on the first time an index calculation moves. */
static int dec_at(int k, uint32_t *asked) {
    const DecSeg *s;
    DCHECK(k >= 0 && k < dec_total(),
           "a decision was read outside the vector — a branch would take an arm this flow never recorded");
    if (k >= g_dec_below) { *asked = g_dec_k[k - g_dec_below]; return g_dec[k - g_dec_below]; }
    for (s = g_dec_base; s; s = s->base)
        if (k >= s->below) { *asked = s->k[k - s->below]; return s->e[k - s->below]; }
    DFAIL("a decision index below the head was not in any frozen segment — the chain's `below` offsets and the "
          "head's origin disagree, so the vector has a hole in it");
    return 0;
}

/* Append one decision to the head, WITH the question it answers. The two are written by the same statement
   because a slot whose arm is recorded and whose question is not is exactly the state this column exists to
   make impossible. */
static void dec_append(int arm, uint32_t asked) {
    dec_ensure(g_dec_n + 1);
    g_dec_k[g_dec_n] = asked;
    g_dec[g_dec_n++] = (signed char)arm;
}

/* AN EMPTY SEGMENT OF `n` SLOTS, counted into the census, with its two columns placed inside ONE block — the
   single speller of the layout the struct describes, because there are three construction sites (a freeze, a
   sibling's arm, and the cold tier's rebuild) and a layout written three times is a layout that disagrees with
   its own free the first time one of them changes. The caller fills `k` and `e`, and sets `below`, `base` and
   `refcount` — the three things that differ between the three sites and nothing else does. */
static DecSeg *dec_seg_alloc(int n) {
    DecSeg *s;

    DCHECK(n > 0, "a decision segment was allocated with no slots — the chain never freezes an empty head, so a "
                  "zero-length segment is one this file did not build and every flow standing on it would "
                  "replay its path one decision short");
    s = reclaim_malloc(sizeof *s);
    CHECK(s, "decide: OOM allocating a decision segment");
    s->k = reclaim_malloc((size_t)n * DEC_SLOT_BYTES);
    CHECK(s->k, "decide: OOM allocating a decision segment's slots");
    /* `e` LIVES INSIDE `k`'s BLOCK, after the whole uint32 column — so `k` carries malloc's alignment and `e`
       needs none. dec_seg_unref frees `s->k` and nothing else; there is no second owner here. */
    s->e = (signed char *)(s->k + n);
    s->n = n;
    s->park_id = -1;   /* unnamed: no park document has written this segment (see the struct) */
    g_dec_seg_live++; g_dec_seg_entries_live += n;
    g_dec_seg_bytes_live += (long)sizeof *s + (long)((size_t)n * DEC_SLOT_BYTES);
    return s;
}

/* THIS FLOW HAS LEFT THE PATH ITS VECTOR DESCRIBES — so the vector ENDS at the cursor, and everything above it
 * belongs to a run this flow is no longer on.
 *
 * IT IS THE MECHANISM decide_arm's key assert used to ask the next reader to build, and the assert's own words
 * are why it is a truncation rather than a widening: "the arm it would let through is another flow's". A replay
 * against TODAY's code and TODAY's replies is ALLOWED to diverge — §Time-travel-resume requires the
 * re-derivation that causes it, and §@S's re-injection point hands a candidate a path recorded by a flow whose
 * source was concolic where the candidate's is concrete, so the candidate legitimately asks FEWER questions.
 * What was missing was not permission to diverge, it was the ability to NOTICE and stop consuming.
 *
 * NOTHING IS DROPPED AND NOTHING IS PRUNED, which is what keeps this out of §NO BOUNDS. The caller forks at
 * this very branch on the line after, so both arms run from here exactly as they would at any new decision;
 * what is abandoned is a RECORD of answers to questions this run did not ask, which no flow can use.
 *
 * IT IS A NEW SEGMENT AND NOT A LOWERED ORIGIN, and that is forced by two sites rather than chosen. `g_dec_below`
 * alone does not describe a flow's vector: decide_resume RE-DERIVES it as `base->below + base->n`, so a
 * truncation held only in the global is undone by the first park; and dec_seg_arm places a sibling's slot at
 * that same `base->below + base->n`, so a fork taken straight after would put the sibling's arm above the tail
 * this flow had just left behind. Freezing the surviving prefix as its own segment makes both of those read
 * the cursor, because the chain is contiguous again — and it is what a fork already does at every other
 * branch, so this is the existing shape rather than a second one.
 * IT COSTS ONE SEGMENT, NEVER THE PREFIX. Only the segment the cursor falls INSIDE is copied — the chain below
 * it is shared by pointer exactly as every fork shares it — so this is O(that segment), which for a
 * fork-per-branch chain is one slot. Copying `[0, g_c)` would be the per-flow `malloc + memcpy` this file's
 * header measures as the quadratic it deleted.
 * THE TAIL IS RELEASED, NEVER TORN OUT. The segments above the cursor stay alive for every sibling standing on
 * them; what is dropped is THIS flow's reference, which is the same bookkeeping a flow finishing performs.
 * THE HEAD IS EMPTY HERE BY CONSTRUCTION: a slot is appended only by the forking branch, which leaves the
 * cursor at the end, so a flow that is still replaying has taken none of its own. Asserted rather than zeroed,
 * because zeroing it would silently discard decisions this flow really did make. */
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
    /* TAKEN BEFORE THE RELEASE, because the release may free `s` and cascade into the very segment this one is
       about to be based on. */
    if (ns->base) ns->base->refcount++;
    g_dec_base = ns;
    g_dec_below = g_c;
    dec_seg_unref(old);
}

/* FREEZE the head into a shared immutable segment and hand the caller ONE reference on it, on top of the
   running flow's own. An empty head freezes to nothing: the caller takes another reference on the chain the
   flow already stands on, which is what keeps a park/resume pair with no decisions between them from pushing an
   empty segment per switch — the same guard concolic_pins_suspend keeps, and for the same reason (the chain's
   depth would then count SWITCHES rather than forks). */
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

/* ONE MORE DECISION over `base`, as its own segment — the sibling's arm at the branch. It takes the caller's
   reference on `base` (the fork's freeze produced exactly one to give).
   `asked` IS THE PARENT'S OWN QUESTION, not a second one: a fork is two arms of ONE branch, so the sibling's
   slot answers the identical predicate and carries the identical key. That is what makes the sibling's replay
   of its one recorded slot checkable at all — it re-executes the branch it was forked over and the key it
   computes there must be the key recorded here. */
static DecSeg *dec_seg_arm(DecSeg *base, int arm, uint32_t asked) {
    DecSeg *s = dec_seg_alloc(1);
    s->e[0] = (signed char)arm;
    s->k[0] = asked;
    s->below = base ? base->below + base->n : 0;
    s->base = base; s->refcount = 1;   /* the fork blob's, and nobody else's until the sibling resumes */
    return s;
}

/* A FRESH FLOW STANDS ON NOTHING — an empty vector, cursor 0, and every branch it reaches is a new decision.
   It used to copy a BIRTH VECTOR off the flow, and no caller ever supplied one: the field was the from-baseline
   replay mechanism, built and never reached, which is exactly the shape the cold tier needed and could not use
   (a flat array per flow is the quadratic the chain above deleted). A flow that carries a recorded path is not
   fresh — it RESUMES, over the chain the cold tier rebuilt, at cursor 0. So there is one representation of a
   decision vector in this engine and it is the shared chain. */
void decide_enter(JSContext *ctx, Flow *f) {
    (void)ctx;
    dec_clear();   /* whatever the previous flow was standing on is not this one's */
    g_dec_n = 0;
    g_c = 0;
    g_cur_fn = f->fn;   /* borrowed */
    g_running = 1;
    concolic_clear_pins();   /* pins are per-flow: this run re-derives them as it replays its EQ gates */
}

void decide_leave(JSContext *ctx) {
    (void)ctx;
    /* THE FLOW IS DONE, so its reference on the chain goes with it. Every other release point is a flow
       STARTING (decide_enter / decide_resume, which clear before installing their own); a flow that finishes
       reaches neither, and the chain it stood on would be held by a global until some later flow happened to
       start. Released where the lifetime actually ends. */
    dec_clear();
    g_running = 0;
    g_cur_fn = JS_UNDEFINED;
}

/* THE SESSION'S TEARDOWN. The head buffer and any chain the last flow left standing are this file's, and
   nothing else frees them — the frontier's teardown frees the flows' BLOBS, which is a different reference. */
void decide_free(void) {
    int i;

    dec_clear();
    free(g_dec); g_dec = NULL;
    free(g_dec_k); g_dec_k = NULL;
    g_dec_cap = 0;   /* ONE capacity for both head buffers — dec_ensure grows them together or not at all */
    for (i = 0; i < DECIDE_FORK_KEYS; i++) { free(g_fork_keys[i].key); g_fork_keys[i].key = NULL; g_fork_keys[i].n = 0; }
    g_fork_other = g_fork_total = 0;
    DCHECK(g_dec_seg_live == 0,
           "the decision chain outlived the session — a frozen segment is referenced by the flows forked below "
           "it and by nothing else, so one still live here is a blob the frontier's teardown did not release");
}

int decide_cursor(void) { return g_c; }

/* WHICH PREDICATE IS GROWING THE FRONTIER — see decide.h.
 *
 * A COUNT AND NOT A LAST-SEEN, because a sample of the most recent fork answers a question nobody asked: a
 * frontier growing at one branch forks there thousands of times and at a dozen other branches once each, and
 * the last one before the sample instant is as likely to be one of the dozen. The table is small and keyed by
 * the constraint key, so two forks at one source and operation are one row however many call sites spell it,
 * and a CHAIN — a source whose operation string carries a position — shows as many rows with one hit each,
 * which is itself the answer. A key that does not fit is counted in the overflow row rather than dropped: an
 * undercount that says so is a measurement, an undercount that does not is a lie. */
/* THE ROW HOLDS THE WHOLE KEY, because a census that truncates its own key merges the rows it exists to tell
   apart — the same defect the key construction itself carried, one layer up, and it would report a frontier
   growing at "one predicate" that is really two. The rows themselves are declared above decide_free. */
static void fork_key_count(const char *key)
{
    int i;

    g_fork_total++;
    for (i = 0; i < DECIDE_FORK_KEYS; i++) {
        if (!g_fork_keys[i].n) {                       /* an empty row: claim it */
            g_fork_keys[i].key = strdup(key);
            CHECK(g_fork_keys[i].key, "decide: OOM recording which predicate is growing the frontier");
            g_fork_keys[i].n = 1;
            return;
        }
        if (!strcmp(g_fork_keys[i].key, key)) { g_fork_keys[i].n++; return; }
    }
    g_fork_other++;
}

const char *decide_fork_at(int i, long *hits)
{
    if (i < 0 || i > DECIDE_FORK_KEYS) return NULL;
    if (i < DECIDE_FORK_KEYS && g_fork_keys[i].n) {
        *hits = g_fork_keys[i].n;
        return g_fork_keys[i].key;
    }
    /* THE OVERFLOW ROW IS A ROW, and it is the one right after the last real one — rows are claimed in order,
       so the FIRST empty index is where the real ones end and is the only index it may appear at. An
       undercount that says so is a measurement; one that does not is a lie. */
    if (g_fork_other && i <= DECIDE_FORK_KEYS && (i == 0 || g_fork_keys[i - 1].n)) {
        *hits = g_fork_other;
        return "(more predicates than this table holds)";
    }
    return NULL;
}

long decide_fork_total(void) { return g_fork_total; }

/* Swap the running decision state when the scheduler interleaves flows. A flow paused mid-execution keeps the
   whole vector it has accumulated (every arm it appended while it ran) + its cursor, so on resume it consumes
   the SAME decisions from where it left off. decide_suspend snapshots; decide_resume restores + re-binds
   cur_fn. It is also the shape the cold tier rebuilds a parked flow into — a chain and a cursor is the whole of
   one flow's decision state whether the chain was frozen by this session or read back out of a recipe. */
/* A BLOB IS A POINTER AT A SEGMENT and a cursor — the whole of one flow's parked decision state, and it is
   O(1) whatever the depth of the vector it names. It was a `malloc(dec_n)` + `memcpy` of the entire vector, on
   every park AND on every fork, which is where the quadratic came from. */
typedef struct { DecSeg *seg; int c; } DecideBlob;
void *decide_suspend(void) {
    DecideBlob *b = reclaim_malloc(sizeof *b); CHECK(b, "decide: OOM suspend blob");
    b->seg = dec_freeze();   /* the blob owns the reference the freeze hands back */
    b->c = g_c;
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
    g_cur_fn = fn;   /* borrowed from the resuming Flow */
    g_running = 1;
}
void decide_blob_free(void *blob) { DecideBlob *b = blob; if (b) { dec_seg_unref(b->seg); free(b); } }

/* THE COLD TIER'S READ AND WRITE OF THIS CHAIN — see decide.h for why the chain is the only part of a snapshot
   that has an identity outside this session's heap, and why the SEGMENTS rather than the flattened vector are
   what crosses. The pointers are opaque to the caller: it names a segment by a park-local ordinal it assigns
   itself, never by anything of this file's. */
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
    /* ONE reference for the base link, exactly as a freeze transfers one, and ONE for the rebuilder — which is
       the only difference between building a chain forwards and freezing one backwards: the rebuilder holds
       every segment it made until the whole park is decoded, because a segment's users are decoded after it. */
    if (b) b->refcount++;
    s->refcount = 1;
    /* A REBUILT SEGMENT IS UNNAMED, and that is not merely bookkeeping: the ordinal a resume reads belongs to
       the document it is READING, and the id here is the name in the document this session will WRITE. A
       resumed flow that is parked again is written out under a fresh ordinal beside every other flow of this
       session, which is the only thing that keeps one park document's ordinals dense in one namespace.
       dec_seg_alloc set it; stated here because this is the site a reader checks it at. */
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
    /* CURSOR 0 — the whole of what makes this a REPLAY. The flow re-runs the document from its first script and
       consumes one recorded arm at each branch it re-reaches; when the cursor catches up with the end of the
       chain it forks like any other flow, which is where its exploration continues. */
    b->c = 0;
    return b;
}

/* See decide.h. `entries` is how many decisions the flow stands on, which is a property of the CHAIN and is
   therefore SHARED with every ancestor; `bytes` is what the flow itself owns, which is now the blob and nothing
   else. Reporting both is the point — the pair is what says the sharing is real. */
void decide_blob_stats(const void *blob, long *entries, long *bytes) {
    const DecideBlob *b = blob;
    if (entries) *entries = (b && b->seg) ? b->seg->below + b->seg->n : 0;
    if (bytes) *bytes = b ? (long)sizeof *b : 0;
}
void decide_live_stats(long *entries, long *bytes) {
    if (entries) *entries = dec_total();
    if (bytes) *bytes = (long)((size_t)g_dec_cap * DEC_SLOT_BYTES);   /* both head buffers; one capacity governs them */
}

/* The sibling's hot decision state at a fork: replay the parent's path so far, then take `arm` at this branch
   (cursor). c = cursor so on resume the sibling re-executes the OP_if and replays exactly this arm — never
   re-forks, never re-runs the prefix. `asked` is the branch's own question, which both arms answer. */
static void *decide_fork_blob(int cursor, int arm, uint32_t asked) {
    DecideBlob *b = reclaim_malloc(sizeof *b); CHECK(b, "decide: OOM fork blob");
    /* A FORK IS ALWAYS AT THE END, and that is what makes the freeze the sibling's whole prefix rather than a
       prefix of it. decide_arm reaches the forking branch only when the cursor has caught up with the vector
       (the replay arm consumes a recorded slot, the constraint arm consumes none and records none), so the head
       being frozen here ends exactly at `cursor`. Asserted rather than assumed: a fork at an interior cursor
       would hand the sibling decisions this flow took AFTER the branch it is being forked at. */
    DCHECK(cursor == dec_total(),
           "a fork was prepared at a cursor that is not the end of the decision vector — the sibling would "
           "inherit decisions taken after the branch it is being forked at");
    b->seg = dec_seg_arm(dec_freeze(), arm, asked);   /* the sibling's arm over the shared prefix; O(1) */
    b->c = cursor;
    return b;
}

/* THE OTHER FORK'S BLOB — see decide.h. No arm, because no question was asked: the sibling's path IS its
   parent's and it diverges over a value that arrived, not over a predicate. The freeze is the same one the
   branch fork performs and for the same reason (one shared immutable prefix, O(1)); the cursor is the parent's
   own, which may be mid-replay.
 *
 * AND THAT LAST CLAUSE IS WHY THIS SITE IS NOT EXEMPT FROM decide_arm's KEY ASSERT, WHICH IT WILL TRIP.
 * The arms below the cursor are what the parent already replayed and agree by construction; the arms ABOVE it
 * are the ones a mid-replay parent has still to consume, and they were recorded by a run in which the peer
 * answered ONE way. This mints N-1 siblings holding N-1 DIFFERENT answers to that read, and every one of them
 * goes on to consume those same arms. At most one of the N can be the timeline that recorded them, so the
 * others replay another timeline's decisions the moment their different answer changes which question the
 * program asks next.
 *
 * THAT IS A DEFECT AND NOT A FALSE POSITIVE, and this file already says so one screen up, in decide.h's own
 * words: "WHAT IT CANNOT CARRY IS THE ANSWER, and that is the honest limit of a decision vector… Recording
 * that mapping is the N-way outcome slot solver_outcome's own DCHECK already names." The key column does not
 * create that limit; it is the first thing in the engine that can SEE it. Narrowing the assert to let this
 * site through would put the limit back under the floorboards and would be the silent fallback §C-stack names
 * — every narrowing condition is a case quietly handed to the broken path. So the assert stands unnarrowed,
 * this site trips it, and the trip is the work queue: an arm forked over a peer's answer needs a slot that
 * records WHICH answer, which is the same N-way slot solver_outcome's n == 2 DCHECK is already waiting for. */
void *decide_fork_same_path(void) {
    DecideBlob *b;

    DCHECK(g_running, "a sibling's decision state was forked while no flow's was loaded — the blob would stand "
                      "on whatever chain the previously-switched-in flow left behind, and the sibling would "
                      "replay a path it never took");
    b = reclaim_malloc(sizeof *b); CHECK(b, "decide: OOM forking a flow's decision state");
    b->seg = dec_freeze();   /* the caller's reference, on top of the running flow's own */
    b->c = g_c;
    /* AND IT IS COUNTED, because this mints a MEMBER OF THE FRONTIER and @PROGRESS's `forks` is the frontier's
       growth. It was counted nowhere: fork_key_count runs only in decide_arm's new-decision branch, so every
       sibling a peer's answer created was invisible to both the total and the histogram — and the histogram's
       own sentence is "WHICH PREDICATE IS GROWING THE FRONTIER", which for this class it could not answer at
       all. The cost of the omission is not the undercount, it is what the undercount does to a reader: a
       frontier's provenance is exactly `created = 1 + forks + candidates + joined documents + cold resumes`,
       and with this class missing the residual of that subtraction is the sum of THREE unrelated things. A sum
       labelled with one of its terms is a number that means whichever term the reader had in mind — measured,
       a residual of 0 was read as "no child document was ever given a timeline", which is not what it says and
       is not even true of the run that produced it.
       IT GETS ITS OWN ROW AND NOT A FABRICATED KEY. There is no predicate here — the paragraph above says so
       in as many words — so composing one would put a branch in the census that the program never took, and
       the row would merge with a real predicate the moment one spelled the same way. The row states what
       actually happened, which is also what keeps the rows summing to the total. */
    fork_key_count("(a peer's answer arrived — no predicate was asked)");
    return b;
}

/* See decide.h. The freeze and the reference are the fork's; the CENSUS ENTRY is not, because no member of the
   frontier is being made. Written as its own function rather than a flag on the one above so that neither
   caller can be read as the other's special case: one mints a flow, one records a path. */
void *decide_freeze_path(void) {
    DecideBlob *b;

    DCHECK(g_running, "a flow's path was frozen while no flow's decision state was loaded — the blob would "
                      "stand on whatever chain the previously-switched-in flow left behind, and every "
                      "candidate seeded from it would replay a path no flow ever took");
    b = reclaim_malloc(sizeof *b); CHECK(b, "decide: OOM freezing a flow's decision path");
    b->seg = dec_freeze();   /* the caller's reference, on top of the running flow's own */
    b->c = g_c;
    return b;
}

/* THE PREDICATE'S IDENTITY, which is what the flow's constraint is keyed by.
 *
 * IT IS THE IDENTITY OF THE VALUE THE BRANCH TESTS, and that one sentence is the whole rule — there is no
 * second case for comparisons. A comparison RESULT is a derived value like any other, and concolic.c composes
 * the operator and BOTH operands into its identity, so `if (x)`, `x < 700`, `x < 300`, `700 < x` and `x > 700`
 * are five identities and five independent facts without this file knowing what any of them mean.
 *
 * IT USED TO KEY A COMPARISON ON THE OPERAND'S SOURCE PLUS AN OPERATOR AND A TOKEN, DEFAULTING BOTH — and the
 * relational hook supplied neither, so `x\x01""\x01""` was every ordering over one source: in one flow
 * `parseInt(gCS(a).width) < 700` and `parseInt(gCS(b).width) < 300` produced the SAME key and the second was
 * decided by the first, pruning an arm the flow's constraint does not contradict. §Solver-half allows exactly
 * one direction ("a contradicted branch is pruned (sound-only — uncertainty keeps the arm)") and that is the
 * other one; a pruned arm emits nothing, so no gate could see it. The `tok ? tok : ""` was not the symptom, it
 * was the CONCEALMENT: it turned "the producer does not produce this" into a plausible key. The consumer now
 * has no field to default, because the whole predicate is the value's own identity.
 *
 * AND IT USED TO BE BUILT INTO A `char key[256]` WITH AN UNCHECKED snprintf, which is the same defect by a
 * second route — concolic_exotic_get's field paths alone were 192 bytes, and a truncated key merges two
 * predicates exactly as a dropped operator does. The composition is sized from its members and cannot
 * truncate, and its length-prefixed fields mean no member can spell another's boundary.
 *
 * Returns a heap key the caller frees, or NULL when the value has no identity this engine can spell —
 * uncertainty, which keeps both arms. */
static char *decide_key(JSValueConst cond) {
    const char *f[1];

    f[0] = concolic_ident_c(cond);
    return concolic_ident_compose("branch", f, 1);
}

/* See decide.h. It reads the constraint WITHOUT touching the decision vector or its cursor: a read is not a
   decision, so it must not consume a slot — the same rule decide_arm's feasible-refinement arm keeps, and for
   the same reason (a consumed slot would make the NEXT branch read someone else's answer). */
int decide_value_arm(JSValueConst cond)
{
    char *key = decide_key(cond);
    int arm;

    if (!key) return -1;
    arm = concolic_branch_decided(key);
    free(key);
    return arm;
}

/* THE RECORDED ARM AT THE CURSOR, or -1 WHEN THE SLOT THERE ANSWERS A DIFFERENT QUESTION — the whole of what
   tells a replay whether it is still on its recorded path, in ONE descent of the chain. The cursor moves only
   on a match, so a caller that gets -1 is standing exactly where the divergence is and dec_leave_path can end
   the vector at it. Asking the question and taking the arm in two calls would be two walks of a chain this
   file's own note already measures as the resume's fastest-growing cost. */
static int dec_replay(uint32_t asked) {
    uint32_t recorded;
    int arm;

    DCHECK(g_c < dec_total(), "the recorded arm was asked for from past the end of the vector — there is no "
                              "slot there to answer with");
    arm = dec_at(g_c, &recorded) ? 1 : 0;
    if (recorded != asked) return -1;
    g_c++;
    return arm;
}

/* THE FORK — reached by a branch this flow has never decided, and by one whose recorded answer belongs to
   another question (dec_leave_path having just ended the vector at the cursor, which makes the two the same
   state). FRAME-SNAPSHOT: prepare the FALSE sibling's hot state (its decision vector + the current
   constraint), and signal the interpreter (0x100 bit) to CLONE this frame at the branch. The sibling resumes
   AT the branch and replays FALSE — it does NOT re-run from the start. This flow takes TRUE. No re-run vector,
   no fallback.
   THE SIBLING'S SNAPSHOT DOES NOT PRE-RECORD ITS ARM, and that deletion is what makes the order in decide_arm
   one rule instead of two. It used to write FALSE into the constraint, take the snapshot, then overwrite with
   TRUE — so the sibling was born already knowing the answer to the branch it was about to re-execute, its
   constraint was AHEAD of its cursor, and the vector had to be consulted before the constraint or the
   sibling's one recorded slot would be read by the NEXT branch. It buys nothing: the sibling resumes AT this
   branch, re-asks it, takes FALSE out of its recorded slot and records FALSE itself on the line below — the
   same constraint, one branch later, derived rather than planted. What the sibling DOES inherit is everything
   this flow knew BEFORE the branch, which is what the snapshot is. */
/* `*forked` COMES BACK FROM THE SEAM AND MEANS "THE CALLER MUST STILL SNAPSHOT", which is not the same
   statement as "a fork happened" and used to be conflated with it. A fork always happens here; whether anything
   is left for the caller to do depends on WHO can build the sibling's resume point (engine_prepare_fork), and
   a caller whose sibling was assembled on the spot must not be told to clone a frame it does not have.
   The parent's own arm is constrained by decide_arm's tail rather than here, so that the constraint is applied
   in ONE place for all three ways a decision is reached — and so that the seam, which may assemble the sibling
   before this returns, sees the parent's constraint exactly as the sibling's frozen copy left it. */
static int dec_fork_here(JSContext *ctx, const char *key, uint32_t asked, int restartable, int *forked) {
    void *dblob = decide_fork_blob(g_c, 0, asked);
    void *pblob;

    fork_key_count(key ? key : "(no source identity)");
    pblob = concolic_pins_suspend();
    *forked = engine_prepare_fork(ctx, dblob, pblob, key, restartable);
    dec_append(1, asked);            /* this flow: TRUE arm, onto the head the freeze above just emptied */
    g_c++;
    return 1;
}

/* THE DECISION ITSELF, over a predicate identified by `key` (NULL when the value tested has no identity this
   engine can spell — uncertainty, which keeps both arms). Every caller of this is a place the program's
   control flow turns on unknown input, and there are two of them because a decision is not an OP_if: a
   BYTECODE BRANCH is one, and a NATIVE OPERATION whose completion depends on the same unknown is the other.
   They share this because they are the same decision — the same vector slot, the same constraint, the same
   replay on a resume — and the only difference is what the interpreter has to do about the arm afterwards. */
/* EVERY SLOT IS WRITTEN AND READ WITH THE QUESTION IT ANSWERS — see the DecSeg struct for why the column
   exists and dec_key_hash for what the name is. The check lives HERE and only here because this is the one
   place a recorded slot is CONSUMED, and an identity that is not compared where it is consumed is a field
   nobody will notice is wrong. */
static int decide_arm(JSContext *ctx, const char *key, int restartable, int *forked) {
    uint32_t asked = dec_key_hash(key);
    int arm;
    *forked = 0;
    if (key && (arm = concolic_branch_decided(key)) >= 0) {
        /* FEASIBLE REFINEMENT, AND IT IS ASKED FIRST. This flow has already decided this exact predicate, so
           the other arm is CONTRADICTED: same unknown input, same test, one answer. Forking it again would add
           a flow that explores nothing and still carries a COW delta, and a bundle testing one flag in twenty
           places would cost a million of them. It consumes NO SLOT, precisely because it adds no decision — it
           is a consequence of the ones already recorded.
           THE ORDER IS THE WHOLE RULE, and it used to be the other way round for a reason that has been fixed
           at its root instead. What the vector records is ONE SLOT PER BRANCH THIS FLOW HAD NOT ALREADY
           DECIDED, so a replay reproduces the original run exactly when it asks the same question in the same
           order: constraint first (no slot), then the recorded arm, then a new fork. Asking the vector first
           only appeared to work because the one replaying flow this engine had — a snapshot-forked sibling —
           replays exactly ONE slot, at a branch whose answer the fork had ALSO pre-recorded into its
           constraint; the two agreed, and the vector had to win or the cursor would fall behind (the case the
           deleted comment named: the false-arm sibling of `cfg.admin` reading the admin slot as its
           `state.beta` decision). That pre-record is gone from the fork below, because the sibling re-executes
           the branch and records the arm itself. With it gone the constraint is never ahead of the cursor, the
           two orders stop competing, and a flow that replays its WHOLE path — the cold tier's, which starts at
           cursor 0 with an empty constraint and re-runs the document — consumes exactly the slots the original
           run recorded. Asking the vector first would have that flow consume a slot at every REPEAT test of a
           flag the original decided for free, and every decision after the first repeat would be some other
           predicate's answer. */
    } else if (g_c < dec_total() && (arm = dec_replay(asked)) >= 0) {   /* REPLAY: a recorded decision */
        /* the cursor moved inside dec_replay, and only because the slot answered THIS question */
    } else {
        /* A NEW DECISION, OR ONE THIS FLOW'S VECTOR NO LONGER DESCRIBES — and after dec_leave_path those are
           the same state, which is why there is one fork and not two.
           THE SLOT MUST ANSWER THE QUESTION NOW BEING ASKED, and until dec_replay could say otherwise the
           replay was POSITIONAL: it took slot g_c whatever the branch was, so a re-run that asked one more, one
           fewer, or the same ones in another order consumed the next predicate's arm here and every
           predicate's arm after it — a real arm, in range, past every assert on this path, all of which are
           RANGE asserts. A clean resume and a scrambled one emitted byte-identical findings, which is why
           nothing ever reported it. The mismatch is now a DIVERGENCE rather than an abort: the recorded tail
           is left behind and this branch forks like any other. */
        if (g_c < dec_total()) dec_leave_path();
        arm = dec_fork_here(ctx, key, asked, restartable, forked);
    }
    /* ONE PLACE, ALL THREE ARMS — a replayed arm and a refined one narrow this flow exactly as a forked one
       does, and the fork path used to state it separately (inside dec_fork_here) so that the `!*forked` guard
       here would not apply it twice. That guard was reading the FORK as its condition; `*forked` now says only
       whether the caller owes a snapshot, which is a different fact, so the statement is made once. It is
       still made ABOVE the freeze the fork took, which is the only ordering this constraint has. */
    if (key) concolic_constrain_branch(key, arm);
    DCHECK(g_c <= dec_total(), "the decision cursor ran past the vector — a branch answered without consuming "
                               "its recorded slot, so the next one will read that slot as its own");
    return arm;
}

/* THE ONE BODY BEHIND BOTH BRANCH ENTRIES — see decide.h. `restartable` is the CALLER's declaration about
   where its sibling comes back, and it is the only thing that differs between them. */
static int decide_branch(JSContext *ctx, JSValueConst cond, int restartable) {
    const char *src = NULL, *tok = NULL;
    char *key;
    int op, forked = 0, arm;

    if (!g_running || !concolic_is(cond)) return -1;   /* not a forced-exec branch on a concolic value */

    /* If the condition is a COMPARISON result (`x === 'admin'`), the taken arm may PIN the source to a concrete
       value — CONCRETIZE-ON-PIN, so later reads compute the REAL @H value, not the shape. */
    op = concolic_cmp(cond, &src, &tok);

    key = decide_key(cond);
    arm = decide_arm(ctx, key, restartable, &forked);
    free(key);

    /* the source equals tok on the arm that makes the EQ true (EQ&&true or NE&&false) -> the code pinned it */
    if (src && tok && ((op == OPCMP_EQ && arm == 1) || (op == OPCMP_NE && arm == 0)))
        concolic_pin(src, tok);
    return forked ? (arm | SOLVER_FORKED_BIT) : arm;   /* the bit tells the interpreter to snapshot-fork this frame */
}

int solver_decide(JSContext *ctx, JSValueConst cond) { return decide_branch(ctx, cond, 0); }

int solver_decide_restartable(JSContext *ctx, JSValueConst cond) {
    int r = decide_branch(ctx, cond, 1);
    DCHECK(r < 0 || !SOLVER_FORKED(r),
           "a restartable branch was told to snapshot a frame — the seam assembles this sibling itself "
           "(there is no activation to clone), so the bit can only mean the seam took the other path and the "
           "caller is about to strand a prepared blob");
    return r;
}

/* JSFlowControlHooks.outcome — the SAME decision asked from a C builtin, which has no OP_if to ask it at.
 *
 * `JSON.parse(x)` over unknown text completes with a value or with a SyntaxError, and both are feasible; the
 * builtin cannot pick one, because picking DELETES the arm a `catch` and everything behind it lives on. So the
 * machine states the question and this answers it out of the flow's decision vector, exactly as a branch's arm
 * comes out of it — which is what makes a sibling parked today and resumed next session take the same arm.
 *
 * The KEY is the operand's IDENTITY plus the OPERATION, composed through the same encoding every other key in
 * this engine goes through — so `x === "a"` and `JSON.parse(x)` are two facts about one value and no spelling
 * of either can compose to the other's key. The operation string is the machine's, so a machine that asks the
 * same question at successive POSITIONS (an iteration over an unknown collection) says so there — each position
 * is its own predicate and must not be decided by the last one's answer. */
int solver_outcome(JSContext *ctx, JSValueConst over, const char *op, int n) {
    if (!g_running) return -1;
    DCHECK(concolic_is(over), "the outcome seam was asked about a value that is not unknown — a native "
                              "operation forks only where its operand's domain permits more than one completion");
    DCHECK(n == 2, "an outcome fork declaring more than two feasible completions — this prepares ONE sibling "
                   "per ask; build the N-1 sibling prepare (a queue engine_fork_finalize drains) before a "
                   "machine declares more");
    {
        const char *f[2];
        char *key;
        int forked = 0, arm;
        DCHECK(concolic_src_c(over) != NULL,
               "an unknown operand with no source identity reached the outcome seam — its completions cannot "
               "be constrained, so two asks about it would be one fact");
        DCHECK(op != NULL,
               "a native operation asked the outcome seam to decide a completion without naming ITSELF — the "
               "operation is half the predicate, and two operations over one operand would be one fact");
        f[0] = concolic_ident_c(over); f[1] = op;
        key = concolic_ident_compose("outcome", f, 2);
        DCHECK(key != NULL, "an operand whose identity this engine cannot spell reached the outcome seam — "
                            "with no key its completions are re-forked at every ask rather than replayed, "
                            "which is sound and is not what a machine declaring a fork expects");
        /* NOT RESTARTABLE, AND THAT IS A STATEMENT ABOUT THE MACHINE RATHER THAN A DEFAULT. An outcome ask
           comes from a C body that is mid-algorithm: re-running the flow's scheduler step would not re-reach
           it, so its sibling's resume point can only be the step driver's snapshot of the machine. */
        arm = decide_arm(ctx, key, 0, &forked);
        free(key);
        return forked ? (arm | SOLVER_FORKED_BIT) : arm;
    }
}
