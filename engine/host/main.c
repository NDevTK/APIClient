/* APIClient v2 host entry — the ONE scheduler's C entry point.
 *
 * Design (rebuild): ONE persistent runtime, ONE top-level scheduler loop, and
 * EVERYTHING is a flow the loop schedules — no --fe-boot/--fe-drive/--fe-deep-grind
 * phases, no separate grind runtime, no second loop. This file starts as the
 * minimal "boot the bundle and eval a script" foundation so the clean quickjs-ng
 * WASM build is verifiable; the scheduler loop + flow registry grow HERE next,
 * in ONE place, so the one-loop invariant is structural.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "quickjs.h"
#include "quickjs-libc.h"

int main(int argc, char **argv)
{
    JSRuntime *rt = JS_NewRuntime();
    if (!rt) { fprintf(stderr, "@E {\"phase\":\"newruntime\"}\n"); return 1; }
    /* Under emscripten callMain the runtime's captured stack-base is stale (JS_NewRuntime ran on a
       different physical stack), so js_check_stack_overflow trips on the FIRST eval. Re-capture the base
       HERE and size the guard under the linker stack (-sSTACK_SIZE). Recursion is reviewed at every depth
       by the scheduler later; this is just the C-stack guard, not a JS depth cap. */
    JS_SetMaxStackSize(rt, 4 * 1024 * 1024);
    JS_UpdateStackTop(rt);
    js_std_init_handlers(rt);
    JSContext *ctx = JS_NewContext(rt);
    if (!ctx) { fprintf(stderr, "@E {\"phase\":\"newcontext\"}\n"); JS_FreeRuntime(rt); return 1; }
    js_std_add_helpers(ctx, argc - 1, argv + 1);
    js_init_module_std(ctx, "std");
    js_init_module_os(ctx, "os");

    /* Milestone 0: eval the script passed as argv[1] (inline source). This proves
       the clean-quickjs-ng WASM build boots + runs JS. The scheduler replaces this
       with: boot-the-bundle-as-flow-0, then the pick/resume/quantum/park loop. */
    int rc = 0;
    if (argc > 1) {
        JSValue v = JS_Eval(ctx, argv[1], strlen(argv[1]), "<boot>", JS_EVAL_TYPE_GLOBAL);
        if (JS_IsException(v)) { js_std_dump_error(ctx); rc = 1; }
        JS_FreeValue(ctx, v);
        js_std_loop(ctx);   /* settle timers/promises — becomes a scheduled flow, not a phase */
    }

    js_std_free_handlers(rt);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    fflush(stdout);
    return rc;
}
