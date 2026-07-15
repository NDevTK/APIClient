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
#include "solver/solve.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* the eval host-edge: a code-execution sink — funnel into the @S solver. */
static JSValue js_eval_sink(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc > 0) solve_eval_sink(ctx, argv[0]);
    return JS_UNDEFINED;
}
/* the innerHTML host-edge: an HTML-context sink (setInnerHTML(x) stands in for el.innerHTML = x). */
static JSValue js_html_sink(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc > 0) solve_html_sink(ctx, argv[0]);
    return JS_UNDEFINED;
}

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
    "fetch('/api/u?uid=' + state.id);"   /* concolic query param -> uid carries {state}.id */
    "if (cfg.admin) { fetch('/api/data?role=admin'); } else { fetch('/api/data?role=public'); }"   /* same endpoint, values MERGE across flows */
    "eval(\"'\" + state.code + \"'\");"   /* @S JS: source lands INSIDE a single-quoted string -> breakout ';X9();// */
    "setInnerHTML('<div>' + state.html + '</div>');"   /* @S HTML: source in HTML text -> breakout an auto-firing element */
    "</script>"
    "</body></html>";

int main(void) {
    JSRuntime *rt = JS_NewRuntime();
    JS_SetMaxStackSize(rt, 4 * 1024 * 1024);   /* align quickjs's overflow check with the emcc 8MB wasm stack */
    JSContext *ctx = JS_NewContext(rt);

    concolic_init(ctx);
    flow_registry_init();
    endpoint_init();
    solve_init(ctx);
    JS_SetBranchHook(solver_decide);

    /* BASELINE setup (mark 0): the globals here must NOT be captured, so install the COW hook AFTER. */
    JSValue g = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, g, "fetch", JS_NewCFunction(ctx, js_fetch, "fetch", 1));
    JS_SetPropertyStr(ctx, g, "eval", JS_NewCFunction(ctx, js_eval_sink, "eval", 1));   /* the eval sink */
    JS_SetPropertyStr(ctx, g, "setInnerHTML", JS_NewCFunction(ctx, js_html_sink, "setInnerHTML", 1));   /* the innerHTML sink */
    JS_SetPropertyStr(ctx, g, "state", concolic_new(ctx, "{state}", "{state}", JS_UNDEFINED));   /* injected/unknown app state */
    JS_FreeValue(ctx, g);

    JS_SetCowHook(cow_capture_hook);   /* per-flow isolation: revert baseline writes made DURING flows */
    JS_SetConcolicAddHook(concolic_add_hook);   /* concolic propagation through `+` (URL building) */

    BootProgram *bp = boot_parse(HTML, strlen(HTML));   /* real Lexbor parse + <script> extraction */
    engine_run(ctx, boot_run, bp);                      /* @H + @S detection */
    solve_verify(ctx, boot_run, bp);                    /* @S: fire-verify the breakout by re-running the real code */
    boot_free(bp);

    /* emit the deduped @H surface — serialized DIRECTLY from the C findings (no JS-object round-trip) */
    char *js = endpoint_json();
    printf("@RESULT %s\n", js);

    int has_uid_param = strstr(js, "\"/api/u\"") && strstr(js, "\"uid\"") && strstr(js, "{state}.id");
    int role_admin = strstr(js, "admin") != NULL;
    int role_public = strstr(js, "public") != NULL;
    int data_count = 0; for (const char *p = js; (p = strstr(p, "\"/api/data\"")); p++) data_count++;
    int merged = (data_count == 1);   /* /api/data appears ONCE with role=[admin,public] merged across flows */

    int h_ok = (has_uid_param && role_admin && role_public && merged);
    printf("@H %s\n", h_ok ? "OK" : "FAIL");

    /* @S: the eval sink reached by concolic state.code, breakout constructed + fire-verified */
    char *ss = solve_json();
    printf("@SEC %s\n", ss);
    /* @S JS: single-quote-context breakout fire-verified. @S HTML: innerHTML sink fire-verified via Lexbor re-parse. */
    int s_eval = strstr(ss, "\"sink\":\"eval\"") && strstr(ss, "{state}.code") && strstr(ss, "';X9();//");
    int s_html = strstr(ss, "\"sink\":\"innerHTML\"") && strstr(ss, "{state}.html") && strstr(ss, "<svg onload=X9()>");
    int s_ok = s_eval && s_html;

    printf("%s\n", (h_ok && s_ok)
        ? "PASS: @H params+merge AND @S eval(';X9();//) + innerHTML(<svg onload=X9()>) — both SEARCHED + fire-verified"
        : "FAIL: @H or @S incorrect");

    free(ss);
    free(js);
    solve_free();
    endpoint_free();

    flow_registry_free(ctx);
    JS_RunGC(rt);   /* collect flow-local garbage from the runs before teardown */
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    return (h_ok && s_ok) ? 0 : 1;
}
