/* The dispatch loop — see engine.h. */
#include "solver/engine.h"
#include "solver/flow.h"
#include "solver/decide.h"
#include "solver/cow.h"
#include "check.h"
#include <string.h>

void flow_exec_once(JSContext *ctx, char *const *bodies, int n) {
    /* Each <script> is its OWN program (separate JS_FlowNew), run in document order in the SAME context — so
       globals (var/function) are shared exactly as in a browser while each script's top-level let/const stays
       scoped to itself. All run under the caller's one COW delta (this code flow's isolated state). */
    for (int i = 0; i < n; i++) {
        JSValue *frame = JS_FlowNew(ctx, bodies[i], strlen(bodies[i]));
        DCHECK(frame != NULL, "flow_exec_once: a page <script> did not compile");
        while (JS_FlowResume(ctx, frame)) { }   /* resume to completion (preemption yields interleaving — later) */
        JS_FlowFree(ctx, frame);
    }
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
