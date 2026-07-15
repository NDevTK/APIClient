/* The dispatch loop — see engine.h. */
#include "solver/engine.h"
#include "solver/flow.h"
#include "solver/decide.h"
#include "solver/cow.h"
#include "check.h"
#include <string.h>

/* Per-run queue of dynamically-loaded script bodies (owned). Rebuilt every replay: a flow that re-takes the
   branch that loads a chunk re-loads it, so the queue is a deterministic function of the flow's decisions. */
static char **g_dyn = NULL; static int g_dyn_n = 0, g_dyn_cap = 0;

void engine_queue_script(const char *body) {
    if (!body) return;
    if (g_dyn_n >= g_dyn_cap) { g_dyn_cap = g_dyn_cap ? g_dyn_cap * 2 : 8; g_dyn = realloc(g_dyn, (size_t)g_dyn_cap * sizeof(char *)); CHECK(g_dyn, "engine: OOM dynamic-script queue"); }
    g_dyn[g_dyn_n] = strdup(body); CHECK(g_dyn[g_dyn_n], "engine: OOM dynamic-script body"); g_dyn_n++;
}

static void run_one(JSContext *ctx, const char *body) {
    JSValue *frame = JS_FlowNew(ctx, body, strlen(body));
    DCHECK(frame != NULL, "flow_exec_once: a script did not compile");
    while (JS_FlowResume(ctx, frame)) { }   /* resume to completion (preemption yields interleaving — later) */
    JS_FlowFree(ctx, frame);
}

void flow_exec_once(JSContext *ctx, char *const *bodies, int n) {
    /* Each <script> is its OWN program (separate JS_FlowNew), run in document order in the SAME context — so
       globals (var/function) are shared exactly as in a browser while each script's top-level let/const stays
       scoped to itself. All run under the caller's one COW delta (this code flow's isolated state). */
    for (int i = 0; i < g_dyn_n; i++) free(g_dyn[i]);
    g_dyn_n = 0;   /* fresh per run — this flow re-loads its own lazy chunks */
    for (int i = 0; i < n; i++) run_one(ctx, bodies[i]);
    /* Drain dynamically-loaded scripts (lazy chunks). Index-based: a chunk may load MORE chunks (transitive),
       appended past g_dyn_n and picked up here. Each runs in THIS flow's globals + COW delta and forks like
       any code, so the lazy surface is explored by the same BFS that explored the static scripts. */
    for (int i = 0; i < g_dyn_n; i++) run_one(ctx, g_dyn[i]);
}

void engine_run(JSContext *ctx, char *const *bodies, int n) {
    flow_add(ctx, JS_UNDEFINED, NULL, 0);   /* the first flow: the page's scripts, empty decision vector */
    JS_SetFlowLocalMark(1);                 /* objects created while a flow runs are flow-local (discarded) */
    Flow *f;
    while ((f = flow_best()) != NULL) {     /* WFQ: highest value-of-information first (all equal at seed -> UCB) */
        f->visits++;
        CowDelta *d = cow_delta_new();       /* this code flow's isolated per-flow COW delta */
        cow_set_current(d);
        decide_enter(ctx, f);
        flow_exec_once(ctx, bodies, n);     /* run each script as its own program; a concolic branch forks siblings */
        decide_leave(ctx);
        cow_set_current(NULL);
        cow_unapply(ctx, d);                 /* restore the baseline this run mutated -> the next flow is isolated */
        cow_delta_free(ctx, d);
        flow_remove(ctx, f);                /* this flow took its arms; the forked siblings carry the rest */
    }
    JS_SetFlowLocalMark(0);
}
