/* The dispatch loop — see engine.h. */
#include "solver/engine.h"
#include "solver/flow.h"
#include "solver/decide.h"
#include "solver/cow.h"
#include "check.h"

void flow_exec_once(JSContext *ctx, const char *src, size_t len) {
    JSValue *frame = JS_FlowNew(ctx, src, len);
    DCHECK(frame != NULL, "flow_exec_once: the page scripts did not compile — boot_source produced invalid JS");
    while (JS_FlowResume(ctx, frame)) { }   /* resume to completion (preemption yields interleaving — later) */
    JS_FlowFree(ctx, frame);
}

void engine_run(JSContext *ctx, const char *src, size_t len) {
    flow_add(ctx, JS_UNDEFINED, NULL, 0);   /* the first flow: the page's scripts, empty decision vector */
    JS_SetFlowLocalMark(1);                 /* objects created while a flow runs are flow-local (discarded) */
    Flow *f;
    while ((f = flow_best()) != NULL) {     /* WFQ: highest value-of-information first (all equal at seed -> UCB) */
        f->visits++;
        CowDelta *d = cow_delta_new();       /* this run's per-flow delta — captures the baseline slots it writes */
        cow_set_current(d);
        decide_enter(ctx, f);
        flow_exec_once(ctx, src, len);      /* run the scripts as a flow; a concolic branch forks siblings */
        decide_leave(ctx);
        cow_set_current(NULL);
        cow_unapply(ctx, d);                 /* restore the baseline this run mutated -> the next flow is isolated */
        cow_delta_free(ctx, d);
        flow_remove(ctx, f);                /* this flow took its arms; the forked siblings carry the rest */
    }
    JS_SetFlowLocalMark(0);
}
