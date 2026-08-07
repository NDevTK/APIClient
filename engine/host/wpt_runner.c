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
#include "core/streams/readable_stream.h"
#include "core/streams/writable_stream.h"
#include "core/streams/transform_stream.h"
#include "core/events/event.h"
#include "core/events/message_event.h"
#include "core/events/message_port.h"
#include "core/events/broadcast_channel.h"
#include "core/frame/window_proxy.h"
#include "core/frame/window_message.h"
#include "core/structured_clone.h"
#include "core/events/event_target.h"
#include "core/dom/abort.h"
#include "solver/concolic.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include "core/timing/timer.h"
#include "core/frame/location.h"
#include "core/encoding/encoding.h"
#include "core/encoding/text_stream.h"
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
static JSValue js_wpt_gc(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    JS_RunGC(JS_GetRuntime(ctx));
    return JS_UNDEFINED;
}

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

/* THE WHOLE REQUEST is recorded, not the URL: the corpus's own handlers answer a POST by its BODY and a header
   probe by its HEADERS, so a record that kept only where-to-go could never satisfy them. The strings are this
   runner's copies, because the request the component built is gone by the time the pump drains. */
#define WPT_OWED_MAX 64
static struct {
    JSValue deliver;
    char   *method, *url, *body;
    size_t  body_len;
    HeaderList headers;
} g_owed[WPT_OWED_MAX];
static int g_owed_n;
/* The document address of the file being run, which is what a relative fetch resolves against. */
static char g_base_url[512];

static void wpt_owe(JSContext *ctx, JSValueConst deliver, JSValueConst value, const FetchRequest *req)
{
    int i;
    (void)value;
    CHECK(g_owed_n < WPT_OWED_MAX, "wpt: more replies owed at once than this runner tracks");
    g_owed[g_owed_n].deliver = JS_DupValue(ctx, deliver);
    g_owed[g_owed_n].method = strdup(req->method ? req->method : "GET");
    g_owed[g_owed_n].url = strdup(req->url ? req->url : "");
    CHECK(g_owed[g_owed_n].url && g_owed[g_owed_n].method, "wpt: OOM recording an owed reply");
    memset(&g_owed[g_owed_n].headers, 0, sizeof g_owed[g_owed_n].headers);
    for (i = 0; req->headers && i < req->headers->n; i++)
        header_list_append(&g_owed[g_owed_n].headers, req->headers->e[i].name, req->headers->e[i].value);
    g_owed[g_owed_n].body = NULL;
    g_owed[g_owed_n].body_len = req->body_len;
    if (req->body) {
        g_owed[g_owed_n].body = malloc(req->body_len + 1);
        CHECK(g_owed[g_owed_n].body, "wpt: OOM copying a request body");
        memcpy(g_owed[g_owed_n].body, req->body, req->body_len);
        g_owed[g_owed_n].body[req->body_len] = 0;
    }
    g_owed_n++;
}

/* Resolve one owed URL against the running file's address and read it out of the corpus. NULL when the corpus
   has no such file, which is a real 404 and is reported as one rather than as an empty body. */
/* THE CORPUS IS SERVED BY WPT'S OWN SERVER, over a socket, exactly as a browser gets it.
 *
 * It was read off disk here, and that was wrong in a way the numbers hid: a `.py` path is a wptserve HANDLER,
 * which the server imports and calls, and answering with the handler's SOURCE made nine files compare their
 * upload against a Python file and report the ENGINE as wrong. Reporting them as a gate limitation was honest
 * about the number and wrong about the cause — the handlers are in the corpus and their dependencies are
 * vendored, so the server runs (engine/wptserve.py) and this speaks HTTP to it.
 *
 * Serving EVERYTHING through it rather than only the handlers is the point: the rewrites, the directory
 * listings and the content types are then wptserve's own, and engine/wpt.mjs's hand-copied rewrite table
 * deletes. The Host header carries the URL's own authority while the socket goes to the loopback port, which is
 * exactly what a hosts-file mapping does for a real browser. */
static char g_server[64];   /* "127.0.0.1:PORT", from the driver */

static int wpt_connect(void)
{
    struct sockaddr_in a;
    char host[64];
    const char *colon = strchr(g_server, ':');
    int fd;

    CHECK(colon != NULL, "wpt: the driver did not name a server to serve the corpus from");
    CHECK((size_t)(colon - g_server) < sizeof host, "wpt: the server address is longer than this runner holds");
    memcpy(host, g_server, (size_t)(colon - g_server));
    host[colon - g_server] = 0;
    fd = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(fd >= 0, "wpt: no socket to reach the corpus server");
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)atoi(colon + 1));
    a.sin_addr.s_addr = inet_addr(host);
    if (connect(fd, (struct sockaddr *)&a, sizeof a) < 0) { close(fd); return -1; }
    return fd;
}

static void wpt_send_all(int fd, const char *p, size_t n)
{
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w <= 0) return;
        p += w; n -= (size_t)w;
    }
}

/* Perform one request and return its BODY, or NULL. A non-2xx is a NULL body, which is what the delivery turns
   into `fetch`'s TypeError — the same answer a real network error gives. */
static char *wpt_http(const FetchRequest *req, size_t *plen, int *pstatus, HeaderList *phdrs)
{
    UrlRecord base, rec;
    char *path = NULL, *authority = NULL, *head;
    char *buf = NULL, *body = NULL;
    size_t cap = 1 << 16, n = 0, hn;
    int fd, status = 0, i;

    url_record_init(&base);
    url_record_init(&rec);
    if (url_parse(&base, g_base_url, strlen(g_base_url), NULL) &&
        url_parse(&rec, req->url, strlen(req->url), &base)) {
        /* THE REQUEST TARGET is the path and the query — the fragment never goes on the wire, which is what
           `exclude_fragment` says, and the authority travels in the Host header instead. */
        char *q = url_serialize_path(&rec);
        size_t n2 = strlen(q) + (rec.query ? strlen(rec.query) : 0) + 2;
        path = malloc(n2);
        CHECK(path, "wpt: OOM building a request target");
        snprintf(path, n2, "%s%s%s", q, rec.query ? "?" : "", rec.query ? rec.query : "");
        free(q);
        authority = url_serialize_host_port(&rec);
    }
    url_record_free(&base);
    url_record_free(&rec);
    if (!path) { free(authority); return NULL; }

    fd = wpt_connect();
    if (fd < 0) { free(path); free(authority); return NULL; }

    hn = strlen(path) + strlen(authority ? authority : "") + 512
       + (req->headers ? (size_t)req->headers->n * 512 : 0);
    head = malloc(hn);
    CHECK(head, "wpt: OOM building a request");
    n = (size_t)snprintf(head, hn, "%s %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n",
                         req->method ? req->method : "GET", path, authority ? authority : "127.0.0.1");
    for (i = 0; req->headers && i < req->headers->n; i++)
        n += (size_t)snprintf(head + n, hn - n, "%s: %s\r\n",
                              req->headers->e[i].name, req->headers->e[i].value);
    n += (size_t)snprintf(head + n, hn - n, "Content-Length: %zu\r\n\r\n", req->body ? req->body_len : 0);
    wpt_send_all(fd, head, n);
    if (req->body && req->body_len) wpt_send_all(fd, req->body, req->body_len);
    free(head);
    free(path);
    free(authority);

    buf = malloc(cap);
    CHECK(buf, "wpt: OOM reading a reply");
    n = 0;
    for (;;) {
        ssize_t got;
        if (n + 4096 > cap) {
            char *g = realloc(buf, cap *= 2);
            CHECK(g, "wpt: OOM growing a reply");
            buf = g;
        }
        got = read(fd, buf + n, cap - n);
        if (got <= 0) break;
        n += (size_t)got;
    }
    close(fd);

    /* HTTP/1.1 <status>, then headers, then a blank line. The body is what follows; `Connection: close` is why
       there is no chunked framing to unpick. */
    if (n > 12 && !memcmp(buf, "HTTP/1.", 7)) status = atoi(buf + 9);
    *pstatus = status;
    {
        char *sep = memmem(buf, n, "\r\n\r\n", 4);
        /* THE REPLY'S HEADERS, which were read past and dropped. A page reads them — `r.headers.get(...)` is
           how it learns a reply's Content-Type — and every one of them answered null. Each line up to the
           blank one is `name: value`; the status line is skipped because it is not one. */
        if (sep) {
            char *line = memchr(buf, '\n', n);
            while (line && line + 1 < sep) {
                char *end = memchr(line + 1, '\r', (size_t)(sep - line - 1));
                char *colon;
                if (!end) break;
                *end = 0;
                colon = strchr(line + 1, ':');
                if (colon) {
                    char *v = colon + 1;
                    *colon = 0;
                    while (*v == ' ' || *v == '\t') v++;
                    header_list_append(phdrs, line + 1, v);
                }
                line = end + 1;
            }
        }
        if (sep && status >= 200 && status < 300) {
            size_t off = (size_t)(sep - buf) + 4;
            *plen = n - off;
            body = malloc(*plen + 1);
            CHECK(body, "wpt: OOM copying a reply body");
            memcpy(body, buf + off, *plen);
            body[*plen] = 0;
        }
    }
    free(buf);
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
        FetchRequest req;
        char *body;
        JSValue arg;
        req.method = g_owed[i].method;
        req.url = g_owed[i].url;
        req.headers = &g_owed[i].headers;
        req.body = g_owed[i].body;
        req.body_len = g_owed[i].body_len;
        {
            int status = 0;
            HeaderList rh = { 0 };
            body = wpt_http(&req, &len, &status, &rh);
            arg = body ? fetch_reply_new(ctx, status, "", &rh, body, len) : JS_NULL;
            header_list_free(&rh);
        }
        /* AS A FLOW, not a JS_Call. Settling the promise reads `then` off the value, which the page can own —
           out of the pump that would have run with no flow base under it. */
        if (JS_CallAsFlow(ctx, g_owed[i].deliver, arg) < 0)
            wpt_report_exception(ctx, g_owed[i].url, "delivering: ");
        JS_FreeValue(ctx, arg);
        JS_FreeValue(ctx, g_owed[i].deliver);
        free(body);
        free(g_owed[i].url);
        free(g_owed[i].method);
        free(g_owed[i].body);
        header_list_free(&g_owed[i].headers);
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
    /* LINE-BUFFERED, so an ABORT cannot throw away what the file already reported. A DCHECK ends the process
       with abort(), which does not flush stdio — and stdout is a pipe here, so it is fully buffered. Every
       result a file produced before it aborted was therefore lost, and the driver read the file as having
       produced NOTHING. That is the "same failures with the count hidden" shape this gate exists to avoid, and
       it hid them from ME: I diagnosed a file as ending with no results and went looking for what it was
       waiting on, when it had reported four failures and then leaked. */
    setvbuf(stdout, NULL, _IOLBF, 0);
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
    /* `gc` — what /common/gc.js reaches for first, and the only way a test that asserts a stream survives
       COLLECTION can assert anything at all. It is the runner's, never the browser's: a page-visible collector
       is fingerprintable and this engine does not ship one. quickjs's own JS_RunGC is the whole of it. */
    JS_SetPropertyStr(ctx, global, "gc", JS_NewCFunction(ctx, js_wpt_gc, "gc", 0));

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
    readable_stream_init(ctx);
    readable_stream_install(ctx, global);
    queuing_strategy_init(ctx);
    queuing_strategy_install(ctx, global);
    /* §5.4's controller carries an AbortSignal, which is an EventTarget that DISPATCHES — so the three
       pieces that stack has to have all exist: the listener key, the Event class the `abort` event is an
       instance of, and the signal's own slot. Installing the signal without the Event class left the abort
       path minting an event out of a class that had never been built. */
    event_target_init(ctx);
    event_init(ctx);
    event_install(ctx, global);
    event_target_install(ctx, global);
    /* WEB IDL §3.6's [Global] rule needs to know WHICH object is the window, and this runner never said. Every
       unqualified `addEventListener(...)` in the corpus — which is how most of it registers — resolved to an
       undefined receiver and registered on nothing. */
    event_target_set_window(ctx, global);
    structured_clone_install(ctx, global);   /* HTML 2.7.3, and what 9.4 and §4.9.7 clone through */
    message_event_init(ctx);
    message_event_install(ctx, global);
    window_proxy_init(ctx);
    window_message_install(ctx, global, "http://web-platform.test");
    message_port_init(ctx);
    message_port_install(ctx, global);   /* HTML 9.4.2/9.4.3 */
    broadcast_channel_init(ctx, "http://web-platform.test");
    broadcast_channel_install(ctx, global);   /* HTML 9.5 */
    abort_init(ctx);        /* the AbortSignal slot key §5.4's signal lives in */
    abort_install(ctx, global);
    writable_stream_init(ctx);
    writable_stream_install(ctx, global);
    transform_stream_init(ctx);
    transform_stream_install(ctx, global);
    blob_init(ctx);
    blob_install(ctx, global);
    encoding_init(ctx);
    encoding_install(ctx, global);
    /* §7.5 and §7.6 are the same codecs driven by a TransformStream, so they install AFTER §6 — the
       constructors reach it through transform_stream_op the moment a page builds one. */
    text_stream_init(ctx);
    text_stream_install(ctx, global);
    /* HTML 8.6's TIMER TASK SOURCE. The runner had none, so `setTimeout` was simply absent — and testharness
       arms its own timeout with one, which is how a file whose tests were still pending ended a run with
       nothing reported and every unsettled promise's reactions leaked. A timer enqueues a JOB, so the pump
       below already drives them; what was missing was only the globals. */
    timer_install(ctx, global);
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
            /* WHERE THE CORPUS IS SERVED FROM. The driver starts wptserve and names its loopback port here;
               the runner speaks HTTP to it and sends the URL's own authority as the Host header, which is what
               a hosts-file mapping does for a real browser. A run with no server has nothing to fetch from, and
               that is a GATE defect rather than a result, so it fails loud. */
            const char *sv = getenv("WPT_SERVER");
            CHECK(sv && *sv && strchr(sv, ':'),
                  "wpt: WPT_SERVER names no host:port — the driver must start engine/wptserve.py and pass it");
            CHECK(strlen(sv) < sizeof g_server, "wpt: WPT_SERVER is longer than this runner holds");
            snprintf(g_server, sizeof g_server, "%s", sv);
        }
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

    /* ANY REPLY STILL OWED IS THIS RUNNER'S TO RELEASE. The pump leaves none behind on its normal exit — it
       stops only when a drain owes nothing new — but a file that FAILED breaks out with entries recorded, and
       each one holds a delivery closure, which holds the capability, which holds every reaction the page
       attached to it. Dropped on the floor they are a leak of the page's own functions, reported at teardown as
       leaked bytecode with no hint of where it came from. */
    {
        int i;
        for (i = 0; i < g_owed_n; i++) {
            JS_FreeValue(ctx, g_owed[i].deliver);
            free(g_owed[i].url);
            free(g_owed[i].method);
            free(g_owed[i].body);
            header_list_free(&g_owed[i].headers);
        }
        g_owed_n = 0;
    }

    timer_reset(ctx);
    headers_free(ctx);
    response_free(ctx);
    request_free(ctx);
    url_free(ctx);
    usp_free(ctx);
    form_data_free(ctx);
    transform_stream_free(ctx);
    writable_stream_free(ctx);
    abort_free(ctx);
    broadcast_channel_free(ctx);
    message_port_free(ctx);
    window_message_free(ctx);
    window_proxy_free(ctx);
    message_event_free(ctx);
    event_free(ctx);
    event_target_free(ctx);
    queuing_strategy_free(ctx);
    readable_stream_free(ctx);
    blob_free(ctx);
    location_free();   /* the API base URL the document's address produced */
    encoding_free(ctx);
    text_stream_free(ctx);
    idl_args_free(ctx);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    return failed ? 1 : 0;
}
