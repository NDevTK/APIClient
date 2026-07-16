/* The frame-agnostic decision — see decide.h. */
#include "solver/decide.h"
#include "solver/concolic.h"
#include "solver/flow.h"
#include "solver/engine.h"
#include "check.h"
#include <stdlib.h>
#include <string.h>

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

/* Take the decision VECTOR out of a fork blob (transfers ownership of the vector, frees the blob struct) — used
   by the frame-agnostic REPLAY fork to seed a fresh (started=0) sibling that re-runs from the flow's fn. */
signed char *decide_blob_take_vector(void *blob, int *n) {
    DecideBlob *b = blob;
    *n = b->dec_n;
    signed char *v = b->dec;   /* transfer ownership to the caller (flow_add frees it with the flow) */
    b->dec = NULL;
    free(b);
    return v;
}

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

int solver_decide(JSContext *ctx, JSValueConst cond) {
    if (!g_running || !concolic_is(cond)) return -1;   /* not a forced-exec branch on a concolic value */

    /* If the condition is a COMPARISON result (`x === 'admin'`), the taken arm may PIN the source to a concrete
       value — CONCRETIZE-ON-PIN, so later reads compute the REAL @H value, not the shape. */
    const char *src = NULL, *tok = NULL;
    int op = concolic_cmp(cond, &src, &tok);

    int arm, forked = 0;
    if (g_c < g_dec_n) {                 /* REPLAY: this run is re-reaching a recorded decision — take that arm */
        arm = g_dec[g_c] ? 1 : 0;
        g_c++;
    } else {
        /* NEW decision -> FRAME-SNAPSHOT FORK: prepare the FALSE sibling's hot state (its decision vector +
           the current pins), and signal the interpreter (0x100 bit) to CLONE this frame at the branch. The
           sibling resumes AT the branch and replays FALSE — it does NOT re-run from the start. This flow takes
           TRUE. No re-run vector, no fallback. */
        engine_prepare_fork(decide_fork_blob(g_c, 0), concolic_pins_suspend());
        dec_ensure(g_c + 1);
        g_dec[g_c] = 1;                  /* this flow: TRUE arm */
        g_dec_n = g_c + 1;
        g_c++;
        arm = 1;
        forked = 1;
    }

    /* the source equals tok on the arm that makes the EQ true (EQ&&true or NE&&false) -> the code pinned it */
    if (src && tok && ((op == OPCMP_EQ && arm == 1) || (op == OPCMP_NE && arm == 0)))
        concolic_pin(src, tok);
    return forked ? (arm | 0x100) : arm;   /* 0x100 tells the interpreter to snapshot-fork this frame */
}
