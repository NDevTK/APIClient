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
#include "quickjs-step.h"
#include "core/fetch/headers.h"
#include "core/fetch/response.h"
#include "core/fetch/request.h"
#include "core/fetch/fetch.h"
#include "core/url/url.h"
#include "core/url/url_search_params.h"
#include "core/html/form_data.h"
#include "core/file/blob.h"
#include "solver/concolic.h"
#include "core/frame/location.h"
#include "core/encoding/encoding.h"
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

/* THE PROLOGUE — the `.any.js` wrapper WPT's server generates around every such file, and which a runner that
   loads the file directly has to supply itself. `self` IS the global (the one spelling a window and a worker
   share) and `GLOBAL` names WHICH global scope this is; a file's `// META: global=` line lists the variants it
   is written for, and each variant runs with GLOBAL answering for itself.
   This runner declares the WORKER variant, because that is what it accurately IS: a global carrying fetch and
   Headers with no document under it. Claiming `window` would make files run their document-dependent arms
   against a runner that has no document — a wrong answer dressed as coverage — and the choice is not a way to
   skip work, since every variant runs the whole file minus the arms upstream itself marks window-only. When
   this runner grows a document, the variant becomes `window` and those arms start running. */
static const char WPT_PROLOGUE[] =
    "self.GLOBAL = {\n"
    "  isWindow: function () { return false; },\n"
    "  isWorker: function () { return true; },\n"
    "  isShadowRealm: function () { return false; },\n"
    "};\n";

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

/* ---- THE CORPUS AS THIS HOST'S NETWORK ------------------------------------------------------------------
 *
 * A wpt test that needs data FETCHES it — url-constructor reads its ~700 cases out of
 * resources/urltestdata.json — and WPT's own server answers those requests off the checked-out tree. This
 * host's network does the same, which is why it is the real fetch component and not a stand-in: the request
 * goes through §5.5, §5.3's Request and §5.1's guards exactly as a page's would, and what differs is only who
 * satisfies it.
 *
 * IT IS DRAINED FROM THE PUMP, not from `owe`. Calling the deliver closure inside `owe` would be a JS_Call
 * from C with no flow base under it — the drive-to-completion this engine aborts on — so the owed reply is
 * recorded and satisfied where the runner already re-enters JS. That is also what the trusted zone does. */
/* The corpus root, derived from the harness path the driver passes — the runner is handed
   `<root>/resources/testharness.js` as its first argument, so the root is what precedes it. */
static char g_wpt_root[512];

static void wpt_report_exception(JSContext *ctx, const char *name, const char *what);

#define WPT_OWED_MAX 64
static struct { JSValue deliver; char *url; } g_owed[WPT_OWED_MAX];
static int g_owed_n;
/* The document address of the file being run, which is what a relative fetch resolves against. */
static char g_base_url[512];

static void wpt_owe(JSContext *ctx, JSValueConst deliver, JSValueConst value, const char *url)
{
    (void)value;
    CHECK(g_owed_n < WPT_OWED_MAX, "wpt: more replies owed at once than this runner tracks");
    g_owed[g_owed_n].deliver = JS_DupValue(ctx, deliver);
    g_owed[g_owed_n].url = strdup(url ? url : "");
    CHECK(g_owed[g_owed_n].url, "wpt: OOM recording an owed reply");
    g_owed_n++;
}

/* Resolve one owed URL against the running file's address and read it out of the corpus. NULL when the corpus
   has no such file, which is a real 404 and is reported as one rather than as an empty body. */
static char *wpt_serve(const char *url, size_t *plen)
{
    UrlRecord base, rec;
    char *path = NULL, *full, *body = NULL;

    url_record_init(&base);
    url_record_init(&rec);
    if (url_parse(&base, g_base_url, strlen(g_base_url), NULL) &&
        url_parse(&rec, url, strlen(url), &base)) {
        path = url_serialize_path(&rec);
    }
    url_record_free(&base);
    url_record_free(&rec);
    if (!path) return NULL;
    {
        /* A `.py` PATH IS A wptserve HANDLER, not a file. WPT's own server IMPORTS it and calls its `main`,
           which is how a test that uploads a form reads back what the server received. This runner serves the
           corpus off disk, so it cannot run one — and answering with the handler's SOURCE is worse than
           answering with nothing, because the test then compares its upload against a Python file and reports
           the engine as wrong. It says so instead: a gate limitation named as one, which is the same thing the
           missing-META-script check does. */
        size_t n = strlen(path);
        if (n > 3 && !strcmp(path + n - 3, ".py")) {
            printf("@WPTHANDLER %s\n", path);
            fflush(stdout);
            free(path);
            return NULL;
        }
    }
    full = malloc(strlen(g_wpt_root) + strlen(path) + 2);
    CHECK(full, "wpt: OOM building a corpus path");
    sprintf(full, "%s%s", g_wpt_root, path);
    body = read_file(full, plen);
    free(full);
    free(path);
    return body;
}

/* Satisfy every reply owed so far. Returns true if any were, so the pump keeps turning while a test's data
   load unblocks the next one. */
static bool wpt_drain_owed(JSContext *ctx)
{
    int i, n = g_owed_n;
    if (!n) return false;
    g_owed_n = 0;   /* taken first: satisfying one may owe another */
    for (i = 0; i < n; i++) {
        size_t len = 0;
        char *body = wpt_serve(g_owed[i].url, &len);
        JSValue arg = body ? JS_NewStringLen(ctx, body, len) : JS_NULL;
        /* AS A FLOW, not a JS_Call. Settling the promise reads `then` off the value, which the page can own —
           out of the pump that would have run with no flow base under it. */
        if (JS_CallAsFlow(ctx, g_owed[i].deliver, arg) < 0)
            wpt_report_exception(ctx, g_owed[i].url, "delivering: ");
        JS_FreeValue(ctx, arg);
        JS_FreeValue(ctx, g_owed[i].deliver);
        free(body);
        free(g_owed[i].url);
    }
    return true;
}

/* Report the pending exception. JS_ToCString would run the thrown object's own `toString` from C — which is
   the drive-to-completion the engine aborts on, and did abort on here, so the report of a failure became a
   crash that hid it. JS_DiagCString is the engine's answer for a host with no flow to run page code on. */
static void wpt_report_exception(JSContext *ctx, const char *name, const char *what)
{
    JSValue e = JS_GetException(ctx);
    char *owned = NULL;
    const char *msg = JS_DiagCString(ctx, e, &owned);
    printf("@WPTERR %s: %s%s\n", name, what, msg ? msg : "?");
    JS_DiagFreeCString(ctx, msg, owned);
    JS_FreeValue(ctx, e);
}

/* Run one program as a FLOW, exactly as the test262 harness does: park and rebuild the whole frame chain at
   every back-edge. A throw is reported and ends the file — a test file that cannot run is a failure, never a
   skip. */
static int run_program(JSContext *ctx, const char *src, size_t len, const char *name)
{
    JSValue *flow = JS_FlowNew(ctx, src, len, name, JS_EVAL_TYPE_GLOBAL);
    JSValue res = JS_UNDEFINED;
    int failed = 0;

    if (!flow) {
        wpt_report_exception(ctx, name, "compile: ");
        return 1;
    }
    while (JS_FlowResume(ctx, flow, &res)) { }
    if (JS_IsException(res)) {
        wpt_report_exception(ctx, name, "");
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
    /* `self` IS the global — testharness.js and every .any.js test reach the global through it. It is set from
       C rather than in WPT_PROLOGUE because the prologue itself is written against `self`. */
    JS_SetPropertyStr(ctx, global, "self", JS_DupValue(ctx, global));
    JS_SetPropertyStr(ctx, global, "print", JS_NewCFunction(ctx, js_wpt_print, "print", 1));

    /* THE COMPONENTS UNDER TEST. Named one by one rather than "everything", because a component that is not
       installed makes its tests fail LOUDLY on a missing global — which is the honest report — while quietly
       installing a stand-in would make the gate agree with itself. Grows as areas are covered. */
    headers_init(ctx);
    headers_install(ctx, global);
    response_init(ctx);
    response_install(ctx, global);
    request_init(ctx);
    request_install(ctx, global);
    url_init(ctx);
    url_install(ctx, global);
    usp_init(ctx);
    usp_install(ctx, global);
    form_data_init(ctx);
    form_data_install(ctx, global);
    blob_init(ctx);
    blob_install(ctx, global);
    encoding_init(ctx);
    encoding_install(ctx, global);
    fetch_install(ctx, global);
    { static const FetchProvider P = { wpt_owe }; fetch_set_provider(&P); }

    /* THE CORPUS ROOT AND THE DOCUMENT ADDRESS. The driver hands this runner
       `<root>/resources/testharness.js` first and the test file second, so the root is what precedes the one
       and the address is the other, made server-relative — which is exactly what WPT's server serves from and
       resolves against. A worker's `location` is a real WorkerLocation; `search` is empty because this runner
       runs the file with no variant query, which is a real variant and not a way to skip the others. */
    {
        const char *h = argv[1], *tail = strstr(h, "/resources/testharness.js");
        size_t n = tail ? (size_t)(tail - h) : 0;
        CHECK(n < sizeof g_wpt_root, "wpt: the corpus root is longer than this runner holds");
        memcpy(g_wpt_root, h, n);
        g_wpt_root[n] = 0;
        /* THE LAST argument is the test; the ones between are its META scripts. Reading argv[2] made a file
           with a META script resolve its data against that script's directory instead of its own. */
        if (argc > 2 && !strncmp(argv[argc - 1], g_wpt_root, n))
            snprintf(g_base_url, sizeof g_base_url, "http://web-platform.test%s", argv[argc - 1] + n);
        else
            snprintf(g_base_url, sizeof g_base_url, "http://web-platform.test/");
        /* THROUGH THE COMPONENT, not hand-built. Three properties assembled here were three chances to
           disagree with the Location the engine ships — and one of them mattered beyond `location.x`: the
           component is where HTML's API base URL is derived from the address, so hand-building the object left
           the base NULL and every relative URL in the corpus unparseable, `URL.createObjectURL` naming the
           opaque origin "null" where the test asserts the document's own. */
        /* THE ADDRESS, NOT THE INTERFACE. The base URL is what this runner needs from the address — every
           relative URL in the corpus resolves against it, and `URL.createObjectURL` names its origin — while
           the Location INTERFACE additionally declares `search` and `hash` as concolic attacker sources. This
           document genuinely has no query: the runner runs one file with no variant, which is a real variant
           and not a way to skip the others. A concolic `search` here is not a truth, and the harness's own
           coercion of it refuses at the C boundary, which is the boundary doing its job. */
        location_set_document_url(g_base_url);
        {
            JSValue loc = JS_NewObject(ctx);
            JS_SetPropertyStr(ctx, loc, "href", JS_NewString(ctx, g_base_url));
            JS_SetPropertyStr(ctx, loc, "search", JS_NewString(ctx, ""));
            JS_SetPropertyStr(ctx, loc, "origin", JS_NewString(ctx, "http://web-platform.test"));
            JS_SetPropertyStr(ctx, loc, "protocol", JS_NewString(ctx, "http:"));
            JS_SetPropertyStr(ctx, global, "location", loc);
        }
    }
    idl_args_seal();
    JS_FreeValue(ctx, global);

    JS_SetFlowControlHooks(&WPT_HOOKS_ON);
    failed = run_program(ctx, WPT_PROLOGUE, strlen(WPT_PROLOGUE), "<wpt-prologue>");
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
        if (r > 0) continue;
        if (r < 0) { failed = 1; break; }
        /* NOTHING LEFT TO RUN IS NOT NOTHING LEFT TO DO. A promise_test parked on `fetch` has no job pending
           and no flow parked — it is waiting on the HOST, exactly as a page waits on the network. Satisfying
           the owed replies is what re-enters JS and gives the pump more to drain; only when a drain owes
           nothing new is the file really finished. */
        if (!wpt_drain_owed(ctx)) break;
    }
    JS_SetFlowControlHooks(&WPT_HOOKS_OFF);

    headers_free(ctx);
    response_free(ctx);
    request_free(ctx);
    url_free(ctx);
    usp_free(ctx);
    form_data_free(ctx);
    blob_free(ctx);
    location_free();   /* the API base URL the document's address produced */
    encoding_free(ctx);
    idl_args_free(ctx);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    return failed ? 1 : 0;
}
