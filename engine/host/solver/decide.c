/* The frame-agnostic decision — see decide.h. */
#include "solver/decide.h"
#include "solver/concolic.h"
#include "solver/flow.h"
#include "check.h"
#include <stdlib.h>
#include <string.h>

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

int solver_decide(JSContext *ctx, JSValueConst cond) {
    if (!g_running || !concolic_is(cond)) return -1;   /* not a forced-exec branch on a concolic value */

    /* If the condition is a COMPARISON result (`x === 'admin'`), the taken arm may PIN the source to a concrete
       value — CONCRETIZE-ON-PIN, so later reads compute the REAL @H value, not the shape. */
    const char *src = NULL, *tok = NULL;
    int op = concolic_cmp(cond, &src, &tok);

    int arm;
    if (g_c < g_dec_n) {                 /* REPLAY: this run is re-reaching a recorded decision — take that arm */
        arm = g_dec[g_c] ? 1 : 0;
        g_c++;
    } else {
        /* NEW decision. Both arms open -> FORK the FALSE sibling (path so far ++ FALSE), this flow takes TRUE.
           The sibling re-runs g_cur_fn replaying (path ++ FALSE). No OP_if rewind (frame-agnostic). */
        signed char *sib = malloc((size_t)(g_c + 1));
        CHECK(sib, "decide: OOM forking the sibling arm — dropping it truncates BFS exploration (loses gated code)");
        if (g_c) memcpy(sib, g_dec, (size_t)g_c);
        sib[g_c] = 0;                    /* sibling: FALSE arm */
        flow_add(ctx, g_cur_fn, sib, g_c + 1);
        dec_ensure(g_c + 1);
        g_dec[g_c] = 1;                  /* this flow: TRUE arm */
        g_dec_n = g_c + 1;
        g_c++;
        arm = 1;
    }

    /* the source equals tok on the arm that makes the EQ true (EQ&&true or NE&&false) -> the code pinned it */
    if (src && tok && ((op == OPCMP_EQ && arm == 1) || (op == OPCMP_NE && arm == 0)))
        concolic_pin(src, tok);
    return arm;
}
