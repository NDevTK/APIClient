/* The frame-agnostic decision — see decide.h. */
#include "solver/decide.h"
#include "solver/concolic.h"
#include "solver/flow.h"
#include "solver/engine.h"
#include "check.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static void *decide_fork_blob(int cursor, int arm);   /* the sibling's hot decision state (defined below) */

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
typedef struct DecSeg { signed char *e; int n, below; struct DecSeg *base; int refcount; } DecSeg;

static DecSeg *g_dec_base = NULL;   /* the frozen prefix the running flow stands on (NULL = it has none) */
static int g_dec_below = 0;         /* slots in that chain — the head's index origin */
static signed char *g_dec = NULL; static int g_dec_n = 0, g_dec_cap = 0;   /* the head: this flow's own */
static int g_c = 0;
static JSValue g_cur_fn = JS_UNDEFINED;   /* borrowed from the running Flow (alive for the run) */
static int g_running = 0;

static void dec_ensure(int n) {
    if (n <= g_dec_cap) return;
    int nc = g_dec_cap ? g_dec_cap * 2 : 64;
    while (nc < n) nc *= 2;
    g_dec = realloc(g_dec, (size_t)nc);
    CHECK(g_dec, "decide: OOM growing the decision vector — depth is bounded only by the RAM/disk floor");
    g_dec_cap = nc;
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
        g_dec_seg_bytes_live -= (long)sizeof *s + s->n;
        free(s->e); free(s);
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
static int dec_at(int k) {
    const DecSeg *s;
    DCHECK(k >= 0 && k < dec_total(),
           "a decision was read outside the vector — a branch would take an arm this flow never recorded");
    if (k >= g_dec_below) return g_dec[k - g_dec_below];
    for (s = g_dec_base; s; s = s->base)
        if (k >= s->below) return s->e[k - s->below];
    DFAIL("a decision index below the head was not in any frozen segment — the chain's `below` offsets and the "
          "head's origin disagree, so the vector has a hole in it");
    return 0;
}

/* Append one decision to the head. */
static void dec_append(int arm) {
    dec_ensure(g_dec_n + 1);
    g_dec[g_dec_n++] = (signed char)arm;
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
    s = malloc(sizeof *s);
    CHECK(s, "decide: OOM freezing the decision vector into a shared segment");
    s->e = malloc((size_t)g_dec_n);
    CHECK(s->e, "decide: OOM freezing the decision vector's entries");
    memcpy(s->e, g_dec, (size_t)g_dec_n);
    s->n = g_dec_n; s->below = g_dec_below; s->base = g_dec_base; s->refcount = 2;   /* running flow + caller */
    g_dec_seg_live++; g_dec_seg_entries_live += s->n;
    g_dec_seg_bytes_live += (long)sizeof *s + s->n;
    g_dec_base = s; g_dec_below = s->below + s->n; g_dec_n = 0;
    return s;
}

/* ONE MORE DECISION over `base`, as its own segment — the sibling's arm at the branch. It takes the caller's
   reference on `base` (the fork's freeze produced exactly one to give). */
static DecSeg *dec_seg_arm(DecSeg *base, int arm) {
    DecSeg *s = malloc(sizeof *s);
    CHECK(s, "decide: OOM recording a sibling's arm");
    s->e = malloc(1);
    CHECK(s->e, "decide: OOM recording a sibling's arm");
    s->e[0] = (signed char)arm;
    s->n = 1; s->below = base ? base->below + base->n : 0;
    s->base = base; s->refcount = 1;   /* the fork blob's, and nobody else's until the sibling resumes */
    g_dec_seg_live++; g_dec_seg_entries_live += 1;
    g_dec_seg_bytes_live += (long)sizeof *s + 1;
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
    dec_clear();
    free(g_dec); g_dec = NULL; g_dec_cap = 0;
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
#define DECIDE_FORK_KEYS 64
static struct { char key[192]; long n; } g_fork_keys[DECIDE_FORK_KEYS];
static long g_fork_other, g_fork_total;

static void fork_key_count(const char *key)
{
    int i;

    g_fork_total++;
    for (i = 0; i < DECIDE_FORK_KEYS; i++) {
        if (!g_fork_keys[i].n) {                       /* an empty row: claim it */
            snprintf(g_fork_keys[i].key, sizeof g_fork_keys[i].key, "%s", key);
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
    DecideBlob *b = malloc(sizeof *b); CHECK(b, "decide: OOM suspend blob");
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

void *decide_seg_new(void *base, const signed char *arms, int n) {
    DecSeg *b = base, *s;

    DCHECK(n > 0, "a decision segment was rebuilt with no arms — every flow standing on it would replay its "
                  "path one decision short, and every branch after that would read the next flow's answer");
    s = malloc(sizeof *s);
    CHECK(s, "decide: OOM rebuilding a parked decision segment");
    s->e = malloc((size_t)n);
    CHECK(s->e, "decide: OOM rebuilding a parked decision segment's arms");
    memcpy(s->e, arms, (size_t)n);
    s->n = n;
    s->below = b ? b->below + b->n : 0;
    s->base = b;
    /* ONE reference for the base link, exactly as a freeze transfers one, and ONE for the rebuilder — which is
       the only difference between building a chain forwards and freezing one backwards: the rebuilder holds
       every segment it made until the whole park is decoded, because a segment's users are decoded after it. */
    if (b) b->refcount++;
    s->refcount = 1;
    g_dec_seg_live++; g_dec_seg_entries_live += s->n;
    g_dec_seg_bytes_live += (long)sizeof *s + s->n;
    return s;
}

void decide_seg_release(void *seg) { dec_seg_unref(seg); }

void *decide_blob_new(void *seg) {
    DecideBlob *b = malloc(sizeof *b);
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
    if (bytes) *bytes = g_dec_cap;
}

/* The sibling's hot decision state at a fork: replay the parent's path so far, then take `arm` at this branch
   (cursor). c = cursor so on resume the sibling re-executes the OP_if and replays exactly this arm — never
   re-forks, never re-runs the prefix. */
static void *decide_fork_blob(int cursor, int arm) {
    DecideBlob *b = malloc(sizeof *b); CHECK(b, "decide: OOM fork blob");
    /* A FORK IS ALWAYS AT THE END, and that is what makes the freeze the sibling's whole prefix rather than a
       prefix of it. decide_arm reaches the forking branch only when the cursor has caught up with the vector
       (the replay arm consumes a recorded slot, the constraint arm consumes none and records none), so the head
       being frozen here ends exactly at `cursor`. Asserted rather than assumed: a fork at an interior cursor
       would hand the sibling decisions this flow took AFTER the branch it is being forked at. */
    DCHECK(cursor == dec_total(),
           "a fork was prepared at a cursor that is not the end of the decision vector — the sibling would "
           "inherit decisions taken after the branch it is being forked at");
    b->seg = dec_seg_arm(dec_freeze(), arm);   /* the sibling's arm over the shared prefix; O(1) */
    b->c = cursor;
    return b;
}

/* THE PREDICATE'S IDENTITY, which is what the flow's constraint is keyed by. A bare truthiness test is about
   the SOURCE, so the source path is the whole key; a comparison is about a source AND what it was compared
   against, so `x === "a"` and `x === "b"` must stay independent facts. The separator is a control character no
   field path contains, so the two key spaces cannot collide. Returns 0 when the condition has no source
   identity to constrain — uncertainty, which keeps both arms. */
static int decide_key(JSValueConst cond, char *buf, size_t n) {
    const char *src = NULL, *tok = NULL;
    int op = concolic_cmp(cond, &src, &tok);
    if (!src) src = concolic_src_c(cond);
    if (!src) return 0;
    if (op == OPCMP_NONE) snprintf(buf, n, "%s", src);
    else snprintf(buf, n, "%s\x01%d\x01%s", src, op, tok ? tok : "");
    return 1;
}

/* See decide.h. It reads the constraint WITHOUT touching the decision vector or its cursor: a read is not a
   decision, so it must not consume a slot — the same rule decide_arm's feasible-refinement arm keeps, and for
   the same reason (a consumed slot would make the NEXT branch read someone else's answer). */
int decide_value_arm(JSValueConst cond)
{
    char key[256];

    if (!decide_key(cond, key, sizeof key)) return -1;
    return concolic_branch_decided(key);
}

/* THE DECISION ITSELF, over a predicate identified by `key` (NULL when the condition has no source identity to
   constrain — uncertainty, which keeps both arms). Every caller of this is a place the program's control flow
   turns on unknown input, and there are two of them because a decision is not the same thing as an OP_if: a
   BYTECODE BRANCH is one, and a NATIVE OPERATION whose completion depends on the same unknown is the other.
   They share this because they are the same decision — the same vector slot, the same constraint, the same
   replay on a resume — and the only difference is what the interpreter has to do about the arm afterwards. */
static int decide_arm(const char *key, int *forked) {
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
    } else if (g_c < dec_total()) {      /* REPLAY: this run is re-reaching a recorded decision — take that arm */
        arm = dec_at(g_c) ? 1 : 0;
        g_c++;
    } else {
        /* NEW decision -> FRAME-SNAPSHOT FORK: prepare the FALSE sibling's hot state (its decision vector +
           the current constraint), and signal the interpreter (0x100 bit) to CLONE this frame at the branch. The
           sibling resumes AT the branch and replays FALSE — it does NOT re-run from the start. This flow takes
           TRUE. No re-run vector, no fallback. */
        void *dblob = decide_fork_blob(g_c, 0);
        fork_key_count(key ? key : "(no source identity)");
        /* THE SIBLING'S SNAPSHOT DOES NOT PRE-RECORD ITS ARM, and that deletion is what makes the order above
           one rule instead of two. It used to write FALSE into the constraint, take the snapshot, then
           overwrite with TRUE — so the sibling was born already knowing the answer to the branch it was about
           to re-execute, its constraint was AHEAD of its cursor, and the vector had to be consulted before the
           constraint or the sibling's one recorded slot would be read by the NEXT branch. It buys nothing: the
           sibling resumes AT this branch, re-asks it, takes FALSE out of its recorded slot and records FALSE
           itself on the line below — the same constraint, one branch later, derived rather than planted. What
           the sibling DOES inherit is everything this flow knew BEFORE the branch, which is what the snapshot
           is. */
        void *pblob = concolic_pins_suspend();
        if (key) concolic_constrain_branch(key, 1);   /* THIS flow's arm, into the head above the shared freeze */
        engine_prepare_fork(dblob, pblob);
        dec_append(1);                   /* this flow: TRUE arm, onto the head the freeze above just emptied */
        g_c++;
        arm = 1;
        *forked = 1;
    }
    if (key && !*forked) concolic_constrain_branch(key, arm);   /* a REPLAYED arm narrows this flow just the same */
    DCHECK(g_c <= dec_total(), "the decision cursor ran past the vector — a branch answered without consuming "
                               "its recorded slot, so the next one will read that slot as its own");
    return arm;
}

int solver_decide(JSContext *ctx, JSValueConst cond) {
    (void)ctx;
    if (!g_running || !concolic_is(cond)) return -1;   /* not a forced-exec branch on a concolic value */

    /* If the condition is a COMPARISON result (`x === 'admin'`), the taken arm may PIN the source to a concrete
       value — CONCRETIZE-ON-PIN, so later reads compute the REAL @H value, not the shape. */
    const char *src = NULL, *tok = NULL;
    int op = concolic_cmp(cond, &src, &tok);

    char key[256];
    int keyed = decide_key(cond, key, sizeof key);
    int forked = 0;
    int arm = decide_arm(keyed ? key : NULL, &forked);

    /* the source equals tok on the arm that makes the EQ true (EQ&&true or NE&&false) -> the code pinned it */
    if (src && tok && ((op == OPCMP_EQ && arm == 1) || (op == OPCMP_NE && arm == 0)))
        concolic_pin(src, tok);
    return forked ? (arm | SOLVER_FORKED_BIT) : arm;   /* the bit tells the interpreter to snapshot-fork this frame */
}

/* JSFlowControlHooks.outcome — the SAME decision asked from a C builtin, which has no OP_if to ask it at.
 *
 * `JSON.parse(x)` over unknown text completes with a value or with a SyntaxError, and both are feasible; the
 * builtin cannot pick one, because picking DELETES the arm a `catch` and everything behind it lives on. So the
 * machine states the question and this answers it out of the flow's decision vector, exactly as a branch's arm
 * comes out of it — which is what makes a sibling parked today and resumed next session take the same arm.
 *
 * The KEY is the operand's SOURCE plus the OPERATION, and the separator is a byte neither a field path nor an
 * operation name contains and is DIFFERENT from the comparison key's: `x === "a"` and `JSON.parse(x)` are two
 * facts about one source, and a key space they shared would let one answer the other. The operation string is
 * the machine's, so a machine that asks the same question at successive POSITIONS (an iteration over an unknown
 * collection) says so there — each position is its own predicate and must not be decided by the last one's
 * answer. */
int solver_outcome(JSContext *ctx, JSValueConst over, const char *op, int n) {
    (void)ctx;
    if (!g_running) return -1;
    DCHECK(concolic_is(over), "the outcome seam was asked about a value that is not unknown — a native "
                              "operation forks only where its operand's domain permits more than one completion");
    DCHECK(n == 2, "an outcome fork declaring more than two feasible completions — this prepares ONE sibling "
                   "per ask; build the N-1 sibling prepare (a queue engine_fork_finalize drains) before a "
                   "machine declares more");
    {
        const char *src = concolic_src_c(over);
        char key[256];
        int forked = 0, arm;
        DCHECK(src != NULL, "an unknown operand with no source identity reached the outcome seam — its "
                            "completions cannot be constrained, so two asks about it would be one fact");
        snprintf(key, sizeof key, "%s\x02%s", src, op ? op : "");
        arm = decide_arm(key, &forked);
        return forked ? (arm | SOLVER_FORKED_BIT) : arm;
    }
}
