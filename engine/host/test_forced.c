/* End-to-end proof of the rebuilt frame-agnostic forced-execution core: a single concolic branch must explore
 * BOTH arms via the dispatch loop, with the fork expressed purely as a decision-vector sibling (no OP_if
 * rewind, no frame snapshot). Runs standalone against clean upstream quickjs + the new solver components. */
#include "quickjs.h"
#include "solver/concolic.h"
#include "solver/decide.h"
#include "solver/flow.h"
#include "solver/engine.h"
#include <stdio.h>
#include <string.h>

static char g_eps[64][128];
static int  g_eps_n = 0;

static JSValue js_fetch(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc > 0) {
        const char *u = JS_ToCString(ctx, argv[0]);
        if (u) { if (g_eps_n < 64) snprintf(g_eps[g_eps_n++], 128, "%s", u); JS_FreeCString(ctx, u); }
    }
    return JS_UNDEFINED;
}

/* the "page": a real MOAT case — `state` is injected/unknown, so state.admin forks and the gated admin
   endpoint surfaces even though the page (logged out) would only ever take the public arm. */
static const char *SCRIPT = "if (state.admin) { fetch('/api/admin'); } else { fetch('/api/public'); }";

static void run_program(JSContext *ctx, void *ud) {
    (void)ud;
    JSValue r = JS_Eval(ctx, SCRIPT, strlen(SCRIPT), "<page>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(r)) {
        JSValue e = JS_GetException(ctx);
        const char *m = JS_ToCString(ctx, e);
        fprintf(stderr, "eval exception: %s\n", m ? m : "?");
        if (m) JS_FreeCString(ctx, m);
        JS_FreeValue(ctx, e);
    }
    JS_FreeValue(ctx, r);
}

int main(void) {
    JSRuntime *rt = JS_NewRuntime();
    JS_SetMaxStackSize(rt, 4 * 1024 * 1024);   /* align quickjs's overflow check with the emcc 8MB wasm stack */
    JSContext *ctx = JS_NewContext(rt);

    concolic_init(ctx);
    flow_registry_init();
    JS_SetBranchHook(solver_decide);

    JSValue g = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, g, "fetch", JS_NewCFunction(ctx, js_fetch, "fetch", 1));
    JS_SetPropertyStr(ctx, g, "state", concolic_new(ctx, "{state}", "{state}", JS_UNDEFINED));   /* injected/unknown app state */
    JS_FreeValue(ctx, g);

    engine_run(ctx, run_program, NULL);

    printf("endpoints reached (%d):\n", g_eps_n);
    for (int i = 0; i < g_eps_n; i++) printf("  %s\n", g_eps[i]);

    int has_admin = 0, has_public = 0;
    for (int i = 0; i < g_eps_n; i++) { if (!strcmp(g_eps[i], "/api/admin")) has_admin = 1; if (!strcmp(g_eps[i], "/api/public")) has_public = 1; }
    printf("%s\n", (has_admin && has_public)
        ? "PASS: gated /api/admin surfaced alongside /api/public — the moat works (concolic source -> fork)"
        : "FAIL: the gated endpoint was not reached");

    flow_registry_free(ctx);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    return (has_admin && has_public) ? 0 : 1;
}
