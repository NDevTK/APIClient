/* End-to-end proof of the rebuilt frame-agnostic forced-execution core: a single concolic branch must explore
 * BOTH arms via the dispatch loop, with the fork expressed purely as a decision-vector sibling (no OP_if
 * rewind, no frame snapshot). Runs standalone against clean upstream quickjs + the new solver components. */
#include "quickjs.h"
#include "solver/concolic.h"
#include "solver/decide.h"
#include "solver/flow.h"
#include "solver/engine.h"
#include "solver/cow.h"
#include "solver/boot.h"
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

/* the "page": a real 2-<script> HTML document. Script 1 reads injected `state` into a config; script 2 (sharing
   globals) branches on it + does a baseline mutation (globalThis.n) first. Exercises: real Lexbor boot,
   cross-script concolic flow (fork on cfg.admin set by script 1), the moat (gated /api/admin), AND per-flow COW
   (both arms see n==1). */
static const char *HTML =
    "<!doctype html><html><body>"
    "<script>var cfg = { admin: state.admin };</script>"
    "<script>"
    "globalThis.n = (globalThis.n || 0) + 1;"
    "if (cfg.admin) { fetch('/api/admin/' + globalThis.n); } else { fetch('/api/public/' + globalThis.n); }"
    "</script>"
    "</body></html>";

int main(void) {
    JSRuntime *rt = JS_NewRuntime();
    JS_SetMaxStackSize(rt, 4 * 1024 * 1024);   /* align quickjs's overflow check with the emcc 8MB wasm stack */
    JSContext *ctx = JS_NewContext(rt);

    concolic_init(ctx);
    flow_registry_init();
    JS_SetBranchHook(solver_decide);

    /* BASELINE setup (mark 0): the globals here must NOT be captured, so install the COW hook AFTER. */
    JSValue g = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, g, "fetch", JS_NewCFunction(ctx, js_fetch, "fetch", 1));
    JS_SetPropertyStr(ctx, g, "state", concolic_new(ctx, "{state}", "{state}", JS_UNDEFINED));   /* injected/unknown app state */
    JS_FreeValue(ctx, g);

    JS_SetCowHook(cow_capture_hook);   /* per-flow isolation: revert baseline writes made DURING flows */

    BootProgram *bp = boot_parse(HTML, strlen(HTML));   /* real Lexbor parse + <script> extraction */
    engine_run(ctx, boot_run, bp);
    boot_free(bp);

    printf("endpoints reached (%d):\n", g_eps_n);
    for (int i = 0; i < g_eps_n; i++) printf("  %s\n", g_eps[i]);

    /* isolation: both arms must see n==1 (COW reverted the other run's mutation) */
    int has_admin = 0, has_public = 0, leak = 0;
    for (int i = 0; i < g_eps_n; i++) {
        if (!strcmp(g_eps[i], "/api/admin/1")) has_admin = 1;
        if (!strcmp(g_eps[i], "/api/public/1")) has_public = 1;
        if (!strcmp(g_eps[i], "/api/admin/2") || !strcmp(g_eps[i], "/api/public/2")) leak = 1;   /* baseline write leaked across flows */
    }
    printf("%s\n", (has_admin && has_public && !leak)
        ? "PASS: gated /api/admin/1 + /api/public/1 — moat works AND baseline write isolated per-flow (COW)"
        : "FAIL: gated endpoint missing or baseline state leaked across flows");

    flow_registry_free(ctx);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    return (has_admin && has_public && !leak) ? 0 : 1;
}
