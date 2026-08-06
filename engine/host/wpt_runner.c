/* THE WEB-PLATFORM-TESTS RUNNER — the browser half's correctness gate.
 *
 * WHY IT EXISTS. test262 is the JS half's oracle and it is a good one: every file carries its own asserts, so a
 * wrong answer is a wrong answer with no interpretation needed. The BROWSER half had nothing of the kind. Its
 * only checks were the IDL audit — which measures COVERAGE, not correctness, and can only ever say what is
 * ABSENT — and a fixture of probes written by whoever wrote the component, which tests what that person already
 * thought of. Both of those missed the same things twice in one sitting: the audit had no row for Headers at
 * all, and then reported it COMPLETE while four of its members were missing.
 *
 * WPT is the established answer, so it is the one used: the real corpus, the real testharness.js, at a pinned
 * revision. A spec-conformance claim about the browser half means "wpt passes", not "our fixture passes".
 *
 * IT RUNS THE FEATURE ENGINE, for the same reason test262 does. There is no non-feature mode in this project, so
 * a conformance pass over a plain JS_Eval would test something that never runs. Each file is a FLOW
 * (JS_FlowNew/JS_FlowResume) under forced preemption — every loop back-edge parks and rebuilds the heap-frame
 * chain — so a Headers iteration that suspends mid-walk is exercised by the same run that checks its answer.
 *
 * The files are given in load order: resources/testharness.js, the test, then an epilogue this runner supplies
 * that registers a completion callback and calls done(). Results come out as one @WPT line per subtest, which
 * the driver reads. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "core/fetch/headers.h"
#include "core/idl_args.h"

/* Forced preemption at every back-edge, sampled at calls — the same policy run-test262 arms, and for the same
   reason: a call is reached orders of magnitude more often than a back-edge, so forcing every one of them buys
   no coverage the sample does not already have and costs a corpus that cannot finish. */
static unsigned g_call_stride;
static int wpt_preempt_always(int kind)
{
    if (kind == JS_PREEMPT_CALL) return (++g_call_stride & 63) == 0;
    return 1;
}
static const JSFlowControlHooks WPT_HOOKS_ON  = { .branch = NULL, .fork = NULL, .preempt = wpt_preempt_always };
static const JSFlowControlHooks WPT_HOOKS_OFF = { .branch = NULL, .fork = NULL, .preempt = NULL };

/* testharness.js reports through a callback; the callback reports through this. One line per subtest, so the
   driver never has to parse prose. */
static JSValue js_wpt_print(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    int i;
    (void)this_val;
    for (i = 0; i < argc; i++) {
        const char *s = JS_ToCString(ctx, argv[i]);
        if (!s) return JS_EXCEPTION;
        fputs(i ? " " : "", stdout);
        fputs(s, stdout);
        JS_FreeCString(ctx, s);
    }
    fputc('\n', stdout);
    return JS_UNDEFINED;
}

static char *read_file(const char *path, size_t *plen)
{
    FILE *f = fopen(path, "rb");
    char *buf;
    long n;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = malloc((size_t)n + 1);
    CHECK(buf, "wpt: OOM reading a test file");
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(buf); return NULL; }
    fclose(f);
    buf[n] = 0;
    *plen = (size_t)n;
    return buf;
}

/* THE EPILOGUE. testharness.js hands its results to a completion callback and, outside a browser, needs to be
   TOLD the page is done — there is no load event here. Both are three lines of the harness's own public API
   rather than anything reimplemented. */
static const char WPT_EPILOGUE[] =
    "add_completion_callback(function (tests, status) {\n"
    "  for (var i = 0; i < tests.length; i++) {\n"
    "    var t = tests[i];\n"
    "    print('@WPT ' + JSON.stringify({ name: t.name, status: t.status, message: t.message }));\n"
    "  }\n"
    "  print('@WPTDONE ' + JSON.stringify({ status: status.status, message: status.message,\n"
    "                                       count: tests.length }));\n"
    "});\n"
    "done();\n";

/* Run one program as a FLOW, exactly as the test262 harness does: park and rebuild the whole frame chain at
   every back-edge. A throw is reported and ends the file — a test file that cannot run is a failure, never a
   skip. */
static int run_program(JSContext *ctx, const char *src, size_t len, const char *name)
{
    JSValue *flow = JS_FlowNew(ctx, src, len, name, JS_EVAL_TYPE_GLOBAL);
    JSValue res = JS_UNDEFINED;
    int failed = 0;

    if (!flow) {
        JSValue e = JS_GetException(ctx);
        const char *msg = JS_ToCString(ctx, e);
        printf("@WPTERR %s: compile: %s\n", name, msg ? msg : "?");
        if (msg) JS_FreeCString(ctx, msg);
        JS_FreeValue(ctx, e);
        return 1;
    }
    while (JS_FlowResume(ctx, flow, &res)) { }
    if (JS_IsException(res)) {
        JSValue e = JS_GetException(ctx);
        const char *msg = JS_ToCString(ctx, e);
        printf("@WPTERR %s: %s\n", name, msg ? msg : "?");
        if (msg) JS_FreeCString(ctx, msg);
        JS_FreeValue(ctx, e);
        failed = 1;
    }
    JS_FreeValue(ctx, res);
    JS_FlowFree(ctx, flow);
    return failed;
}

int main(int argc, char **argv)
{
    JSRuntime *rt;
    JSContext *ctx;
    JSValue global;
    int i, failed = 0;

    if (argc < 2) {
        fprintf(stderr, "usage: wpt_runner <testharness.js> <test.js> [more.js ...]\n");
        return 2;
    }
    rt = JS_NewRuntime();
    JS_SetMaxStackSize(rt, 4 * 1024 * 1024);
    ctx = JS_NewContext(rt);

    global = JS_GetGlobalObject(ctx);
    /* `self` IS the global — testharness.js and every .any.js test reach the global through it, because that is
       the one spelling shared by a window and a worker. */
    JS_SetPropertyStr(ctx, global, "self", JS_DupValue(ctx, global));
    JS_SetPropertyStr(ctx, global, "print", JS_NewCFunction(ctx, js_wpt_print, "print", 1));

    /* THE COMPONENTS UNDER TEST. Named one by one rather than "everything", because a component that is not
       installed makes its tests fail LOUDLY on a missing global — which is the honest report — while quietly
       installing a stand-in would make the gate agree with itself. Grows as areas are covered. */
    headers_init(ctx);
    headers_install(ctx, global);
    idl_args_seal();
    JS_FreeValue(ctx, global);

    JS_SetFlowControlHooks(&WPT_HOOKS_ON);
    for (i = 1; i < argc && !failed; i++) {
        size_t len = 0;
        char *src = read_file(argv[i], &len);
        if (!src) { printf("@WPTERR %s: cannot read\n", argv[i]); failed = 1; break; }
        failed = run_program(ctx, src, len, argv[i]);
        free(src);
    }
    if (!failed)
        failed = run_program(ctx, WPT_EPILOGUE, strlen(WPT_EPILOGUE), "<wpt-epilogue>");
    /* The jobs are the test's too — a promise_test's body runs in one — so they drain with the feature still
       armed, for the reason run-test262 keeps its hooks on across the drain.
       PARKED FLOWS RESUME FIRST. A flow that suspended is ahead of every queued job in program order, so
       draining a job while one is parked reorders observable microtasks — and this pump got it wrong on its
       first run, which the engine's own assert named at the site with the fix in the message. */
    for (;;) {
        JSContext *c1;
        int r;
        while (JS_ResumeParkedFlow(rt)) ;
        r = JS_ExecutePendingJob(rt, &c1);
        if (r <= 0) { if (r < 0) failed = 1; break; }
    }
    JS_SetFlowControlHooks(&WPT_HOOKS_OFF);

    headers_free(ctx);
    idl_args_free(ctx);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    return failed ? 1 : 0;
}
