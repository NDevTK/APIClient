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

/* The RUNNING flow's decision state. g_dec is the vector being replayed/extended; g_c is the cursor (decisions
   consumed this run); g_cur_fn is the function a sibling fork re-drives. The scheduler owns the lifecycle via
   decide_enter/leave. */
static signed char *g_dec = NULL; static int g_dec_n = 0, g_dec_cap = 0;
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

void decide_enter(JSContext *ctx, Flow *f) {
    (void)ctx;
    dec_ensure(f->dec_n);
    if (f->dec_n) memcpy(g_dec, f->dec, (size_t)f->dec_n);
    g_dec_n = f->dec_n;
    g_c = 0;
    g_cur_fn = f->fn;   /* borrowed */
    g_running = 1;
    concolic_clear_pins();   /* pins are per-flow: this run re-derives them as it replays its EQ gates */
}

void decide_leave(JSContext *ctx) {
    (void)ctx;
    g_running = 0;
    g_cur_fn = JS_UNDEFINED;
}

int decide_cursor(void) { return g_c; }

/* Swap the running decision state when the scheduler interleaves flows. A flow paused mid-execution keeps its
   evolving vector (forks may have extended g_dec past f->dec) + its cursor, so on resume it consumes the SAME
   decisions from where it left off. decide_suspend snapshots; decide_resume restores + re-binds cur_fn. */
typedef struct { signed char *dec; int dec_n, c; } DecideBlob;
void *decide_suspend(void) {
    DecideBlob *b = malloc(sizeof *b); CHECK(b, "decide: OOM suspend blob");
    b->dec = malloc(g_dec_n ? (size_t)g_dec_n : 1); CHECK(b->dec, "decide: OOM suspend vector");
    if (g_dec_n) memcpy(b->dec, g_dec, (size_t)g_dec_n);
    b->dec_n = g_dec_n; b->c = g_c;
    return b;
}
void decide_resume(void *blob, JSValueConst fn) {
    DecideBlob *b = blob;
    dec_ensure(b->dec_n);
    if (b->dec_n) memcpy(g_dec, b->dec, (size_t)b->dec_n);
    g_dec_n = b->dec_n; g_c = b->c;
    g_cur_fn = fn;   /* borrowed from the resuming Flow */
    g_running = 1;
}
void decide_blob_free(void *blob) { DecideBlob *b = blob; if (b) { free(b->dec); free(b); } }

/* The sibling's hot decision state at a fork: replay the parent's path so far, then take `arm` at this branch
   (cursor). c = cursor so on resume the sibling re-executes the OP_if and replays exactly this arm — never
   re-forks, never re-runs the prefix. */
static void *decide_fork_blob(int cursor, int arm) {
    DecideBlob *b = malloc(sizeof *b); CHECK(b, "decide: OOM fork blob");
    b->dec_n = cursor + 1;
    b->dec = malloc((size_t)b->dec_n); CHECK(b->dec, "decide: OOM fork vector");
    if (cursor) memcpy(b->dec, g_dec, (size_t)cursor);
    b->dec[cursor] = (signed char)arm;
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
    if (g_c < g_dec_n) {                 /* REPLAY: this run is re-reaching a recorded decision — take that arm */
        arm = g_dec[g_c] ? 1 : 0;
        g_c++;
    } else if (key && concolic_branch_decided(key) >= 0) {
        /* FEASIBLE REFINEMENT. This flow has already decided this exact predicate, so the other arm is
           CONTRADICTED: same unknown input, same test, one answer. Forking it again would add a flow that
           explores nothing and still carries a COW delta, and a bundle testing one flag in twenty places would
           cost a million of them.
           It is checked HERE and not earlier, which is the whole of its correctness. A branch with a RECORDED
           decision owns a slot in the vector — it was a real fork when it was first taken — and answering it
           from the constraint instead would leave the cursor behind, so the NEXT branch would read that slot as
           its own answer. That is exactly what it did: the false-arm sibling of `cfg.admin` replayed the admin
           decision as its `state.beta` decision and one of the four combinations vanished. A constraint decides
           only a branch that would OTHERWISE BE A NEW FORK, and it consumes no slot precisely because it adds
           no decision — it is a consequence of the ones already recorded, and it re-derives identically on
           every resume from the constraint that rides the flow beside that vector. */
        arm = concolic_branch_decided(key);
    } else {
        /* NEW decision -> FRAME-SNAPSHOT FORK: prepare the FALSE sibling's hot state (its decision vector +
           the current constraint), and signal the interpreter (0x100 bit) to CLONE this frame at the branch. The
           sibling resumes AT the branch and replays FALSE — it does NOT re-run from the start. This flow takes
           TRUE. No re-run vector, no fallback. */
        void *dblob = decide_fork_blob(g_c, 0);
        /* the sibling's constraint must already say FALSE when its snapshot is taken — it is the arm the
           sibling IS, so a later test of the same predicate over there is decided too. Recorded, snapshotted,
           then overwritten with this flow's arm; the two orders are one statement apart and getting them
           backwards hands the sibling this flow's answer. */
        if (key) concolic_constrain_branch(key, 0);
        void *pblob = concolic_pins_suspend();
        if (key) concolic_constrain_branch(key, 1);
        engine_prepare_fork(dblob, pblob);
        dec_ensure(g_c + 1);
        g_dec[g_c] = 1;                  /* this flow: TRUE arm */
        g_dec_n = g_c + 1;
        g_c++;
        arm = 1;
        *forked = 1;
    }
    if (key && !*forked) concolic_constrain_branch(key, arm);   /* a REPLAYED arm narrows this flow just the same */
    DCHECK(g_c <= g_dec_n, "the decision cursor ran past the vector — a branch answered without consuming its "
                           "recorded slot, so the next one will read that slot as its own");
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
    return forked ? (arm | 0x100) : arm;   /* 0x100 tells the interpreter to snapshot-fork this frame */
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
        return forked ? (arm | 0x100) : arm;
    }
}
