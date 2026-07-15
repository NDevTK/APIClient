/* The dispatch loop — see engine.h. */
#include "solver/engine.h"
#include "solver/flow.h"
#include "solver/decide.h"
#include "solver/cow.h"

void engine_run(JSContext *ctx, void (*run_program)(JSContext *ctx, void *ud), void *ud) {
    flow_add(ctx, JS_UNDEFINED, NULL, 0);   /* boot: the first flow, empty decision vector */
    JS_SetFlowLocalMark(1);                 /* objects created while a flow runs are flow-local (discarded) */
    Flow *f;
    while ((f = flow_best()) != NULL) {     /* WFQ: highest value-of-information first (all equal at seed -> UCB) */
        f->visits++;
        decide_enter(ctx, f);
        run_program(ctx, ud);               /* execute once; a concolic branch forks siblings into the frontier */
        decide_leave(ctx);
        cow_revert(ctx);                     /* restore the baseline this run mutated -> the next flow is isolated */
        flow_remove(ctx, f);                /* this flow took its arms; the forked siblings carry the rest */
    }
    JS_SetFlowLocalMark(0);
}
