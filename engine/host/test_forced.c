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
#include "solver/endpoint.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* the fetch host-edge: funnel into the real @H endpoint surface (dedup + shape happen there). */
static JSValue js_fetch(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc > 0) {
        const char *method = "GET", *mc = NULL;
        if (argc > 1 && JS_IsObject(argv[1])) {
            JSValue m = JS_GetPropertyStr(ctx, argv[1], "method");
            if (JS_IsString(m)) { mc = JS_ToCString(ctx, m); if (mc) method = mc; }
            JS_FreeValue(ctx, m);
        }
        endpoint_record(ctx, method, argv[0]);
        if (mc) JS_FreeCString(ctx, mc);
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
    "fetch('/api/region/' + state.region);"   /* concolic URL built by concat -> shape must carry {state}.region */
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
    endpoint_init();
    JS_SetBranchHook(solver_decide);

    /* BASELINE setup (mark 0): the globals here must NOT be captured, so install the COW hook AFTER. */
    JSValue g = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, g, "fetch", JS_NewCFunction(ctx, js_fetch, "fetch", 1));
    JS_SetPropertyStr(ctx, g, "state", concolic_new(ctx, "{state}", "{state}", JS_UNDEFINED));   /* injected/unknown app state */
    JS_FreeValue(ctx, g);

    JS_SetCowHook(cow_capture_hook);   /* per-flow isolation: revert baseline writes made DURING flows */
    JS_SetConcolicAddHook(concolic_add_hook);   /* concolic propagation through `+` (URL building) */

    BootProgram *bp = boot_parse(HTML, strlen(HTML));   /* real Lexbor parse + <script> extraction */
    engine_run(ctx, boot_run, bp);
    boot_free(bp);

    /* emit the deduped @H surface — serialized DIRECTLY from the C findings (no JS-object round-trip) */
    char *js = endpoint_json();
    printf("@RESULT %s\n", js);

    int has_admin = strstr(js, "/api/admin/1") != NULL;
    int has_public = strstr(js, "/api/public/1") != NULL;
    int has_shape = strstr(js, "/api/region/{state}.region") != NULL;
    int leak = strstr(js, "/api/admin/2") != NULL || strstr(js, "/api/public/2") != NULL;
    int region_count = 0; for (const char *p = js; (p = strstr(p, "/api/region/")); p++) region_count++;
    int deduped = (region_count == 1);   /* the concolic URL is recorded ONCE despite running per-flow */

    printf("%s\n", (has_admin && has_public && has_shape && !leak && deduped)
        ? "PASS: deduped @H surface — moat + COW + concolic shape + dedup, emitted in C (no leak)"
        : "FAIL: @H surface wrong (missing endpoint / leak / not deduped)");

    free(js);
    endpoint_free();

    flow_registry_free(ctx);
    JS_RunGC(rt);   /* collect flow-local garbage from the runs before teardown */
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    return (has_admin && has_public && has_shape && !leak && deduped) ? 0 : 1;
}
