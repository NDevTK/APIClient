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
 * AND THE EXECUTION MODEL IT RUNS IS THE PRODUCT'S, WHICH IS A STRONGER STATEMENT AND IS TRUE OF HALF OF THIS
 * FILE SO FAR. Parking a frame is not the same thing as being scheduled: a `while (JS_FlowResume)` exercises
 * the park-and-rebuild machinery while the SCHEDULER never runs, so a gate built on one measures a model the
 * product does not have. The CHILD DOCUMENT — this process re-executed for a cross-origin navigable — is on
 * the one scheduler: engine_sched_begin seeds its document's scripts on the frontier, engine_sched_step is the
 * only thing that enters JS, engine_route attaches a delivery to every live timeline and engine_perform makes a
 * peer's operation a program of each of them. The TOP-LEVEL half still drives its programs itself and still
 * pumps quickjs's global job list; run_program names the two scheduler capabilities that stand between it and
 * the same treatment, and the top-level flow it fabricates in main() names itself.
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
#include "core/file/file_system.h"
#include "core/file/file_system_access.h"
#include "core/file/file_picker.h"
#include "core/file/file_system_handle.h"
#include "core/file/file_system_writable.h"
#include "core/streams/readable_stream.h"
#include "core/streams/writable_stream.h"
#include "core/streams/transform_stream.h"
#include "core/streams/queuing_strategy.h"
#include "core/events/event.h"
#include "core/events/error_event.h"
#include "core/events/message_event.h"
#include "core/events/report_exception.h"
#include "core/events/message_port.h"
#include "core/xhr/xml_http_request.h"
#include "core/events/broadcast_channel.h"
#include "core/frame/window.h"
#include "core/frame/window_proxy.h"
#include "core/frame/remote_object.h"
#include "core/frame/remote_op.h"
#include "core/html/html_iframe.h"
#include "core/frame/navigable.h"
#include "solver/world.h"
#include "solver/cow.h"
#include "solver/dom_cow.h"
#include "core/frame/window_message.h"
#include "core/structured_clone.h"
#include "core/events/event_target.h"
#include "core/platform.h"
#include "core/realm.h"
#include "core/dom/abort.h"
#include "core/dom/observable.h"
#include "solver/concolic.h"
#include "solver/flow.h"
#include "solver/engine.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include "core/timing/event_loop.h"
#include "core/timing/timer.h"
#include "core/css/media_query_list.h"
#include "core/frame/viewport.h"
#include "core/frame/visual_viewport.h"
#include "core/rendering/animation_frame.h"
#include "core/rendering/page_reveal.h"
#include "core/rendering/rendering.h"
#include <lexbor/html/html.h>
#include "core/dom/element.h"
#include "core/dom/document.h"
#include "core/loader/document_scripts.h"
#include "core/frame/history.h"
#include "core/frame/navigation.h"
#include "core/frame/navigation_history_entry.h"
#include "core/frame/session_history.h"
#include "core/frame/location.h"
#include "core/encoding/encoding.h"
#include "core/encoding/text_stream.h"
#include "core/idl_args.h"
#include "core/idl_async_iter.h"

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
/* WHERE A DIAGNOSTIC GOES, and why it is not always stdout. In the parent, stdout carries the test results the
   driver parses. In a CHILD DOCUMENT, stdout IS THE ANSWER PIPE — one line per question — so a `print()` from
   the page or an @WPTERR from a failed read would be read by the parent as the answer to its question, and the
   protocol desynchronises from there: the next answer is one line behind, and the run hangs at teardown on a
   read that will never come. A child has no results to report, so its diagnostics go to stderr, which the
   driver already merges into the file's output. */
static FILE *g_report;
static FILE *report_out(void) { return g_report ? g_report : stdout; }

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
        fputs(i ? " " : "", report_out());
        fputs(s, report_out());
        JS_FreeCString(ctx, s);
    }
    fputc('\n', report_out());
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
        /* THE BODY OF EVERY RESPONSE, NOT ONLY A 2xx ONE. Fetch rejects a promise for a NETWORK ERROR and for
           nothing else — a 404 RESOLVES, with `ok` false and the server's error page as the body — so cutting
           the body off at the status made `fetch("/missing")` reject where the standard resolves. XHR is what
           made it impossible to ignore: `status` and `responseText` on a 404 are exactly what half of
           xhr/ asserts, and there is no second helper to give them to. A reply with no header terminator at
           all is still the network error it always was. */
        if (sep) {
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
            /* §4.1's clone of the REQUEST's URL list. This runner serves the checked-out corpus and follows no
               redirect, so the list is the one URL it was asked for — which is what makes `response.url` the
               address the test fetched and `response.redirected` false, both computed rather than declared. */
            arg = body ? fetch_reply_new(ctx, status, "", &rh, body, len, &req.url, 1) : JS_NULL;
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
/* THE PENDING EXCEPTION, REPORTED. The `wpt_report_thrown` half that used to stand beside this — a thrown value
   the caller already held — existed for exactly one caller, the cross-agent operation whose throw is its ANSWER
   rather than a failure; that operation is performed by the scheduler now (flow_answer_perform encodes the
   completion with its type), so the split has nothing left on the other side of it and goes with it. */
static void wpt_report_exception(JSContext *ctx, const char *name, const char *what)
{
    JSValue e = JS_GetException(ctx);
    char *owned = NULL;
    const char *msg = JS_DiagCString(ctx, e, &owned);

    fprintf(report_out(), "@WPTERR %s: %s%s\n", name, what, msg ? msg : "?");
    JS_DiagFreeCString(ctx, msg, owned);
    JS_FreeValue(ctx, e);
}

/* THE HOST'S HALF, pumped between resumes.
 *
 * NOTICES ARE ONE-WAY and are drained here. `navigable.create` announces a document the engine has already
 * named and already handed the page a WindowProxy for, so there is nothing to answer — the engine mints its own
 * child document names (world.h), which is what makes §4.8.5's insertion-step creation synchronous.
 *
 * WHAT THIS RUNNER STILL OWES A CHILD DOCUMENT IS AN INSTANCE, and it cannot provide one: one document per
 * instance, and the components hold per-document singletons, so a second document cannot live in this process.
 * The consequence is exact rather than vague — a read that needs the child's ACTIVE DOCUMENT parks its flow and
 * the file times out, which is visible; a read that needs only the NAVIGABLE (self, parent, top, opener,
 * closed, name) is answered inside the creating instance and never reaches here at all.
 *
 * SO THERE IS NO REQUEST LOOP. There was one, and every branch in it has been deleted rather than emptied: a
 * loop that walks the owed requests and answers none of them is not a place to add the missing capability, it
 * is a place to be tempted into guessing an answer — which is what "not created" was. An unanswered request
 * stays on the flow's register and is reported by qjs_host_requests to whoever can eventually route it. */
/* ---- A SECOND DOCUMENT IS A SECOND PROCESS ---------------------------------------------------------------
 *
 * SECURITY.md says one WASM instance per DOCUMENT, and CLAUDE.md says a document may be remote REGARDLESS OF
 * ORIGIN and must never be co-located to dodge the transport. This runner is a process rather than a wasm
 * instance, so the faithful reading is: a child navigable is a child PROCESS — the same binary, re-executed
 * with the child's name, address, origin and policy, talking over a pipe.
 *
 * CO-LOCATING IT WOULD HAVE BEEN EASIER AND WRONG. Twenty components hold per-document singletons, so a second
 * document in this process is not merely awkward — it is a second `document`, a second element-wrapper table
 * and a second world registry sharing one set of statics. But that is not the reason: the reason is that the
 * transport is the thing that has to work. The cross-instance COW delta rides it, the origin stamp rides it,
 * and a design that answers a cross-document read without one has tested nothing about either.
 *
 * WHAT CROSSES IS TEXT, and that is the same constraint the real seam has: a live JSValue crosses neither a
 * process, nor a wasm instance, nor a session, nor a park. The answer carries its TYPE because `otherW.length`
 * is a number and a page compares it with `===` — an answer that arrived as the string "0" would satisfy a
 * loose check and prove only that bytes moved. */
#define WPT_CHILD_MAX 32
/* THE TOP-LEVEL DOCUMENT'S ORIGIN, named once because it is now TWO facts: the origin its own document is
   installed with, and the origin this host STAMPS on a message it routes out of that document. */
#define WPT_TOP_ORIGIN "http://web-platform.test"

typedef struct { char *name; char *origin; int to, from; pid_t pid; } ChildDoc;
static ChildDoc g_children[WPT_CHILD_MAX];
static int g_children_n;
static const char *g_self_exe;   /* argv[0], which is what a child navigable is re-executed from */

static ChildDoc *wpt_child_for(const char *name)
{
    int i;
    for (i = 0; i < g_children_n; i++)
        if (!strcmp(g_children[i].name, name)) return &g_children[i];
    return NULL;
}

/* §7.4/§4.8.5's create, from the host's side: provision an instance for a document the engine has already
   named and already handed the page a WindowProxy for. */
static void wpt_spawn_child(const char *name, const char *url, const char *origin, const char *csp,
                            const char *top_level_url)
{
    int down[2], up[2];
    pid_t pid;

    if (wpt_child_for(name)) return;   /* already provisioned; a notice is not a request */
    CHECK(g_children_n < WPT_CHILD_MAX, "wpt: more child documents at once than this runner tracks");
    /* O_CLOEXEC, AND IT IS NOT HYGIENE. fork() copies every descriptor, so a SECOND child would inherit this
       parent's write end to the FIRST child's stdin — and then closing the parent's copy never gives that
       child EOF, its serve loop never ends, and teardown blocks in waitpid forever. The whole run hung after
       its last result, which reads as the engine failing to finish rather than as a leaked descriptor.
       dup2 clears the flag on the fd it creates, so the child's own 0 and 1 survive the exec and the
       originals do not. */
    CHECK(pipe2(down, O_CLOEXEC) == 0 && pipe2(up, O_CLOEXEC) == 0,
          "wpt: could not open the pipes to a child document");
    pid = fork();
    CHECK(pid >= 0, "wpt: could not fork a child document");
    if (pid == 0) {
        /* THE CHILD. stdin carries the questions, stdout the answers — and stdout is therefore NOT where its
           test output goes, because a child document has none: it runs a page, not a test file. */
        dup2(down[0], 0);
        dup2(up[1], 1);
        close(down[0]); close(down[1]); close(up[0]); close(up[1]);
        execl(g_self_exe, g_self_exe, "--document", name, url, origin, csp ? csp : "",
              top_level_url ? top_level_url : "", (char *)NULL);
        _exit(127);
    }
    close(down[0]); close(up[1]);
    g_children[g_children_n].name = strdup(name);
    /* ITS ORIGIN, kept because this host STAMPS it on everything that child emits — the child cannot name its
       own sender any more than the top-level document can. */
    g_children[g_children_n].origin = strdup(origin ? origin : "null");
    CHECK(g_children[g_children_n].name != NULL && g_children[g_children_n].origin != NULL,
          "wpt: OOM recording a child document");
    g_children[g_children_n].to = down[1];
    g_children[g_children_n].from = up[0];
    g_children[g_children_n].pid = pid;
    g_children_n++;
}

/* Ask a child document one question and read one answer line. False when the child is gone, which is a real
   answer for the caller to report rather than a reason to hang. */
/* THE CHILD'S OWN NOTICES COME BACK UP THIS PIPE, prefixed `N\t`, before its answer. A child instance cannot
   initiate — the transport is one question, one answer — so without this a message it POSTS is emitted into a
   channel nobody drains and vanishes: the reply half of every messaging protocol, silently absent. The relay
   is what makes the child's emissions reach the one router that knows where they go, and routing one may ask
   another child, which is why this is re-entrant on a different child's pipes. */
static void wpt_route_notice(JSContext *ctx, char *line, const char *from_origin);
static JSContext *g_wpt_ctx;   /* the top-level document's realm, for a notice relayed while a read is in flight */

static bool wpt_child_read_line(ChildDoc *c, char **out, size_t *cap)
{
    size_t n = 0;
    for (;;) {
        char ch;
        ssize_t r = read(c->from, &ch, 1);
        if (r <= 0) return false;
        if (ch == '\n') break;
        if (n + 2 > *cap) {
            size_t want = *cap ? *cap * 2 : 256;
            char *g = realloc(*out, want);
            CHECK(g != NULL, "wpt: OOM reading a child document's answer");
            *out = g; *cap = want;
        }
        (*out)[n++] = ch;
    }
    if (!*cap) { *out = malloc(1); CHECK(*out != NULL, "wpt: OOM"); *cap = 1; }
    (*out)[n] = 0;
    return true;
}

/* Ask one question and return the answer line, BORROWED until the next ask. It used to copy into a caller's
   fixed buffer, which silently truncated the moment a record carried a value — an encoded string is base64 and
   an argument list has no length — and a truncated answer decodes as a different value with nothing to say so. */
static const char *wpt_child_ask(ChildDoc *c, const char *op)
{
    static char *buf;
    static size_t buf_cap;

    wpt_send_all(c->to, op, strlen(op));
    wpt_send_all(c->to, "\n", 1);
    for (;;) {
        if (!wpt_child_read_line(c, &buf, &buf_cap)) return NULL;
        if (!strncmp(buf, "N\t", 2)) {
            /* A NOTICE THE CHILD EMITTED, routed by the zone that knows where it goes — and stamped with the
               CHILD's origin, because it is the child that sent it. */
            char *line = strdup(buf + 2);
            CHECK(line != NULL, "wpt: OOM relaying a child's notice");
            DCHECK(g_wpt_ctx != NULL, "a child relayed a notice before this host had a document to route it "
                                      "into — the top-level realm names where a reply is delivered");
            wpt_route_notice(g_wpt_ctx, line, c->origin);
            free(line);
            continue;
        }
        return buf;
    }
}

static void wpt_children_free(void)
{
    int i, st;
    for (i = 0; i < g_children_n; i++) {
        close(g_children[i].to);
        close(g_children[i].from);
        waitpid(g_children[i].pid, &st, 0);
        free(g_children[i].name);
    }
    g_children_n = 0;
}

/* Defined below, next to the other GETs against the corpus server — §7.4 step 14's fetch is answered here and
   it is the same request every other one is. */
static char *wpt_get_csp(const char *url, size_t *plen, char **pcsp);

/* Returns whether it ANSWERED anything, because the pump has to know. A flow parked on a host-owed answer is
   resumed by JS_ResumeParkedFlow, re-parks on the same unanswered request and is resumed again — so a pump
   that resumes before it answers spins at full CPU until something outside kills it. Every other owed thing in
   this file already reports that (wpt_drain_owed, wpt_run_pending, timer_run_due); this one did not, because
   while the only host request was an occasional cross-document read it was only ever pulled from inside
   run_program's own resume loop, where the answer landed first by luck of ordering. §7.4 step 14's load parks
   on one, and luck ran out. */
/* THE TRUSTED ZONE'S ROUTER. Every notice, whoever emitted it, comes through here — the top-level document's
   own, and every one relayed up from a child instance — because "which instance holds which document" is
   exactly the one fact only this process knows, and a second copy of the decision would be a second answer.
   `from_origin` is the origin of the instance that EMITTED the notice, which this host knows and the emitting
   engine may not name for itself: it is what a routed message is stamped with. `line` is modified in place. */
static void wpt_route_notice(JSContext *ctx, char *line, const char *from_origin);

/* Relay one message to whichever instance holds `doc` — a child, or this process's own document.
   `record` is the emitting engine's notice VERBATIM. It used to be taken apart here and written back out in a
   shape of this host's own — the doc field dropped, the sender origin spliced into its place — which is
   window_message.c's grammar restated where nothing can check it against the writer, and it is also what made
   the receiving side unable to use the engine's own entry for it. The zone's job is to ROUTE the record and to
   STAMP the origin the emitting engine may not name for itself (SECURITY.md); the origin therefore travels
   BESIDE the record rather than inside it, and the record crosses untouched. */
static void wpt_route_post(JSContext *ctx, const char *doc, const char *record, const char *from_origin,
                           const char *world, const char *target_origin, const char *b64)
{
    ChildDoc *c = wpt_child_for(doc);

    if (c) {
        size_t cap = strlen(from_origin) + strlen(record) + 16;
        char *op = malloc(cap);
        CHECK(op != NULL, "wpt: OOM routing a message to a child document");
        snprintf(op, cap, "post\t%s\t%s", from_origin, record);
        wpt_child_ask(c, op);   /* a bare ack; nothing is asked of the receiver */
        free(op);
        return;
    }
    /* NOT A CHILD, SO IT IS THIS PROCESS'S OWN DOCUMENT — the reply direction, which is most of what a real
       messaging protocol does: a frame answers its opener. The delivery is the same one a child makes, in this
       instance, so it goes through the same engine entry rather than a second path. */
    DCHECK(!strcmp(doc, "wpt"),
           "a routed message named a document neither this process nor any child of it holds — the "
           "navigable.create notice for it was dropped, or the post outran it");
    {
        /* THE RECORD IS TAKEN APART BY THE FILE THAT WROTE IT — window_message_route for the payload, and
           world_parse for the vector. This host used to do both by hand (strchr for the colon, a decode of its
           own), which is the writer's grammar restated where nothing can check it against the writer. */
        WorldId w, anc[16];
        size_t cap = strlen(target_origin) + strlen(b64) + 2;
        char *tail = malloc(cap);

        CHECK(tail != NULL, "wpt: OOM receiving a routed message");
        snprintf(tail, cap, "%s\t%s", target_origin, b64);
        world_parse(world, &w, anc, (int)(sizeof anc / sizeof anc[0]));
        window_message_route(ctx, tail, world_doc_name(w.doc), from_origin);
        free(tail);
    }
}

static void wpt_route_notice(JSContext *ctx, char *line, const char *from_origin)
{
    char *f[7]; int nf = 0; char *q;
    /* THE RECORD THE EMITTING ENGINE WROTE, kept whole before this router takes it apart. The split below
       writes NULs into `line`, and a routed post crosses to its instance VERBATIM — the receiving engine's own
       entry parses it, so a rejoin of the fields here would be this host restating a grammar it does not own. */
    char *whole = strdup(line);

    CHECK(whole != NULL, "wpt: OOM keeping a host notice whole");
    /* THE SPLIT STOPS AT THE LAST FIELD AND KEEPS THE REMAINDER VERBATIM, because the last field of a
       navigable.create IS a raw CSP header and HTTP allows HTAB inside one. */
    for (q = line, f[nf++] = q; *q && nf < 7; q++)
        if (*q == '\t') { *q = 0; f[nf++] = q + 1; }
    if (nf == 7 && !strcmp(f[0], "navigable.create")) {
        /* FIELD 5 IS HTML §8.1.3.1's TOP-LEVEL CREATION URL, which crosses for the same reason field 6's
           policy container does: the child's environment is decided by the operation that created it, and the
           instance that will host the child has no way to derive it. */
        wpt_spawn_child(f[1], f[3], f[4], f[6], f[5]);
        free(whole);
        return;
    }
    /* `windowproxy.post <target doc> <world> <target origin> <base64>` — §9.4.4 across instances. THE ORIGIN
       IS STAMPED HERE, by the zone that knows who sent it: the engine may not name its own sender
       (SECURITY.md), so the notice carries none and this adds the one it knows. */
    if (nf == 5 && !strcmp(f[0], "windowproxy.post")) {
        wpt_route_post(ctx, f[1], whole, from_origin, f[2], f[3], f[4]);
        free(whole);
        return;
    }
    free(whole);
    DFAIL("a host notice reached the router under an operation it does not route — a notice this host drops is "
          "a capability the engine emitted for and nothing performs, which is invisible at the emitting end");
}

static bool wpt_answer_host_requests(JSContext *ctx)
{
    const char *notices = engine_host_notices();
    const char *p;
    bool answered = false;

    /* THE NOTICES FIRST: a document announced but not yet provisioned is one a read would arrive for before
       its instance exists. */
    for (p = notices; *p; ) {
        const char *end = strchr(p, '\n');
        char *line;
        size_t n;

        if (!end) break;
        n = (size_t)(end - p);
        /* THE BUFFER IS SIZED TO THE RECORD. It was a fixed 1024 with the over-long ones SKIPPED, which is a
           silent drop of exactly the notices that carry payload — a routed message is base64 and routinely
           longer than that, and a dropped one is a delivery the peer never makes with nothing said. */
        line = malloc(n + 1);
        CHECK(line != NULL, "wpt: OOM reading a host notice");
        memcpy(line, p, n); line[n] = 0;
        wpt_route_notice(ctx, line, WPT_TOP_ORIGIN);
        free(line);
        p = end + 1;
    }

    /* AND THE REQUESTS, each routed to the instance holding that document — which is what the trusted zone
       does in the extension, for the same reason: only it knows which instance holds which document. */
    for (p = engine_host_requests(); *p; ) {
        const char *tab = strchr(p, '\t');
        const char *end = strchr(p, '\n');
        char op[512];
        uint32_t id;
        size_t n;

        if (!tab || !end) break;
        id = (uint32_t)strtoul(p, NULL, 10);
        n = (size_t)(end - tab - 1);
        /* EVERY CROSS-AGENT OPERATION ROUTES THE SAME WAY — a member of a navigable's Window, and the four
           internal methods a lent object performs ([[Get]], [[Set]], [[Delete]], [[Call]]). They differ in what
           the peer resolves, not in who resolves it, which is why this is a prefix and not a list: an operation
           added to remote_object.c reaches its instance with nothing here to remember. */
        if (!strncmp(tab + 1, "windowproxy.get\t", 16) || !strncmp(tab + 1, "object.", 7)) {
            /* THE RECORD IS RELAYED WHOLE, so it is copied at ITS OWN LENGTH. It was copied into a 512-byte
               stack buffer with the over-long ones SKIPPED, which parks the asking flow forever the first time
               an operation carries a value — an encoded string is base64 and an argument list has no length. */
            char *rec = malloc(n + 1), *doc, *q;
            ChildDoc *c;
            CHECK(rec != NULL, "wpt: OOM relaying a cross-agent operation");
            memcpy(rec, tab + 1, n); rec[n] = 0;
            doc = strchr(rec, '\t');
            q = doc ? strchr(doc + 1, '\t') : NULL;
            DCHECK(q != NULL, "a cross-agent operation arrived with no document field — every one of them names "
                              "the instance that owns the object as its first operand");
            if (q) {
                *q = 0;
                c = wpt_child_for(doc + 1);
                *q = '\t';
                /* AN OPERATION FOR A DOCUMENT NOTHING PROVISIONED is not a slow answer, it is a missing
                   instance — the notice that named it was dropped, or the operation outran it. Left unanswered
                   the flow parks forever and the file times out with nothing said, so it says it. */
                DCHECK(c != NULL, "a cross-agent operation named a document this host never provisioned — the "
                                  "navigable.create notice for it was dropped, or this is a CHILD instance "
                                  "asking its PARENT, which is the reverse direction of a transport that has "
                                  "only one: a child answers, it never initiates");
                if (c) {
                    /* THE ZONE'S RENDEZVOUS TOKEN travels with the operation, because the instance that
                       performs it answers by EMITTING a completion that names one (engine.h's engine_perform)
                       rather than by returning a value to whoever called it. This channel carries one question
                       at a time, so the asking request's own id is a token that cannot collide on it — and it
                       is minted HERE, by the zone, for the reason engine_perform gives: a request id is unique
                       only inside the instance that minted it, and two peers may hand this one the same
                       number. */
                    size_t opcap = strlen(rec) + 32;
                    char *ask = malloc(opcap);
                    const char *answer;
                    CHECK(ask != NULL, "wpt: OOM asking a child document to perform an operation");
                    snprintf(ask, opcap, "op\t%lu\t%s", (unsigned long)id, rec);
                    answer = wpt_child_ask(c, ask);
                    free(ask);
                    if (answer) {
                        /* THE ANSWER'S GRAMMAR IS remote_object.c's, in both processes and both directions —
                           an object name says WHICH agent's namespace it is in, so a name that came home
                           resolves to the original object rather than to a proxy of it. AND AN ANSWER IS A
                           COMPLETION: the peer ran a program, so it may have thrown, and this zone relays the
                           type rather than deciding anything about it — the asking flow's own machine raises
                           the throw at the call site that parked on it. */
                        int completion = ENGINE_COMPLETION_NORMAL;
                        JSValue v = remote_completion_decode(ctx, answer, &completion);
                        engine_host_answer(ctx, id, v, completion);
                        answered = true;
                        JS_FreeValue(ctx, v);
                    }
                }
            }
            free(rec);
        }
        /* §7.4 STEP 14'S FETCH, which this runner answers itself because the document being loaded belongs to
           the instance that asked — routing it to a peer would be answering a same-origin load out of another
           agent. `{body, csp}` is ONE answer because a policy is a property of THE RESPONSE; a body of null is
           a fetch that did not load, which is still a document the navigable gets. */
        else if (!strncmp(tab + 1, "document.fetch\t", 15)) {
            char *csp = NULL, *body;
            size_t len = 0;
            JSValue v = JS_NewObject(ctx);

            DCHECK(n < sizeof op, "a document address outgrew this runner's request buffer — truncating it "
                                  "would GET a different URL and skipping it parks the loading flow forever, "
                                  "so the buffer is what has to grow");
            memcpy(op, tab + 1, n); op[n] = 0;
            body = wpt_get_csp(strchr(op, '\t') + 1, &len, &csp);
            JS_SetPropertyStr(ctx, v, "body",
                              body ? JS_NewStringLen(ctx, body, len) : JS_NULL);
            JS_SetPropertyStr(ctx, v, "csp", csp ? JS_NewString(ctx, csp) : JS_NULL);
            free(body);
            free(csp);
            /* THE HOST'S OWN ANSWER, so a NORMAL completion: this zone fetched bytes, it did not run a peer's
               program, and a load that did not load is `{body: null}` rather than a throw. */
            engine_host_answer(ctx, id, v, ENGINE_COMPLETION_NORMAL);
            answered = true;
            JS_FreeValue(ctx, v);
        }
        /* XHR §3.5.6's FETCH, which this runner answers with the same HTTP it answers every other request
           with. The record crosses as JSON because it carries a method, a header list and a body — a
           tab-separated line cannot hold a body — and because the answer already crosses that way.
           IT IS ANSWERED HERE AND NOT ROUTED TO A PEER: an XMLHttpRequest belongs to the instance that made
           it, and the trusted zone's job for one is the network, not the routing. */
        else if (!strncmp(tab + 1, "xhr.send\t", 9)) {
            char *line = malloc(n + 1);
            JSValue rec, mv, uv, hv, bv, reply;
            const char *method, *url, *bodys = NULL;
            size_t blen = 0;
            HeaderList hdrs = { 0 }, rh = { 0 };
            FetchRequest req;
            uint32_t hn = 0, hi;
            char *body;
            size_t len = 0;
            int status = 0;

            CHECK(line != NULL, "wpt: OOM reading an XMLHttpRequest's request record");
            memcpy(line, tab + 1, n); line[n] = 0;
            rec = JS_ParseJSON(ctx, line + 9, strlen(line + 9), "<xhr.send>");
            free(line);
            DCHECK(!JS_IsException(rec), "an XMLHttpRequest's request record did not arrive as JSON — the "
                                         "engine built it with JS_JSONStringify, so a parse failure here is a "
                                         "truncation between the two");
            if (JS_IsException(rec)) { JS_FreeValue(ctx, JS_GetException(ctx)); p = end + 1; continue; }
            mv = JS_GetPropertyStr(ctx, rec, "method");
            uv = JS_GetPropertyStr(ctx, rec, "url");
            hv = JS_GetPropertyStr(ctx, rec, "headers");
            bv = JS_GetPropertyStr(ctx, rec, "body");
            method = JS_ToCString(ctx, mv);
            url = JS_ToCString(ctx, uv);
            if (JS_IsString(bv)) bodys = JS_ToCStringLen(ctx, &blen, bv);
            { JSValue lv = JS_GetPropertyStr(ctx, hv, "length");
              JS_ToUint32(ctx, &hn, lv);
              JS_FreeValue(ctx, lv); }
            for (hi = 0; hi < hn; hi++) {
                JSValue pair = JS_GetPropertyUint32(ctx, hv, hi);
                JSValue nv2 = JS_GetPropertyUint32(ctx, pair, 0), vv2 = JS_GetPropertyUint32(ctx, pair, 1);
                const char *nm = JS_ToCString(ctx, nv2), *val = JS_ToCString(ctx, vv2);
                if (nm && val) header_list_append(&hdrs, nm, val);
                if (nm) JS_FreeCString(ctx, nm);
                if (val) JS_FreeCString(ctx, val);
                JS_FreeValue(ctx, nv2); JS_FreeValue(ctx, vv2); JS_FreeValue(ctx, pair);
            }
            req.method = method ? method : "GET";
            req.url = url ? url : "";
            req.headers = &hdrs;
            req.body = bodys;
            req.body_len = blen;
            body = wpt_http(&req, &len, &status, &rh);
            /* §4.1's clone of the REQUEST's URL list — see the fetch drain above; XHR's `responseURL` is the
               same fact and comes from the same place. */
            reply = body ? fetch_reply_new(ctx, status, "", &rh, body, len, &req.url, 1) : JS_NULL;
            /* Also the host's own — §3.5.6's fetch, answered out of the network; a network error is a reply
               this component reads, never a thrown value. */
            engine_host_answer(ctx, id, reply, ENGINE_COMPLETION_NORMAL);
            answered = true;
            JS_FreeValue(ctx, reply);
            free(body);
            header_list_free(&rh);
            header_list_free(&hdrs);
            if (method) JS_FreeCString(ctx, method);
            if (url) JS_FreeCString(ctx, url);
            if (bodys) JS_FreeCString(ctx, bodys);
            JS_FreeValue(ctx, mv); JS_FreeValue(ctx, uv); JS_FreeValue(ctx, hv); JS_FreeValue(ctx, bv);
            JS_FreeValue(ctx, rec);
        }
        /* An operation this harness does not route is left UNANSWERED rather than guessed: the asking flow
           stays parked, which is visible, where a wrong answer is not. */
        p = end + 1;
    }
    return answered;
}

/* ---- THE TEST FILE, WHICH IS OFTEN A DOCUMENT ----------------------------------------------------------
 *
 * MORE THAN HALF THE CORPUS IS AN HTML FILE, and this gate ran none of them: it collected `.any.js` and
 * `.window.js` and nothing else, so 523 of the 778 test files checked out here were not run, not reported, and
 * not counted. That is an exclusion, and an excluded test is a failure whatever the reason for excluding it —
 * a directory whose files are never collected is the same defect as a directory that is never checked out,
 * except that the total LOOKS complete.
 *
 * A `.window.js` is a script WPT's server wraps in a minimal document. An `.html` file IS the document, and the
 * difference is only which one this runner parses: after that, both run their scripts against the same Window.
 *
 * IT IS FETCHED, NOT READ OFF DISK. A `.sub.html` is SUBSTITUTED by wptserve (hosts, ports, the local origin),
 * so the bytes on disk are a template and running them would test the template. The runner already speaks HTTP
 * to the corpus's own server for everything a test fetches; the document itself goes the same way, which is
 * also what a browser does. */
/* WHICH IT IS, THE DRIVER SAYS — this runner does not guess from the file name. It guessed `.html` and only
 * `.html`, which was a second copy of engine/wpt.mjs's collection rule; the corpus spells a document `.htm`
 * and `.xhtml` too, so the two copies disagreed for 26 checked-out files and the disagreement would have been
 * a document executed as a script. The driver already decided — it collected the file BECAUSE it is a markup
 * document that loads testharness.js — so it passes `--test-document` and there is one rule, in one place. */
static int    g_html_mode;      /* the test is a document, not a script */
static char   g_test_url[512];  /* its address, server-relative — what to GET and what to resolve against */
/* THE VARIANT IS PART OF THE ADDRESS, which is the whole of what a variant IS. WPT's manifest emits one test
   per `<meta name=variant>` / `// META: variant=` declaration, each at the file's own URL with that query or
   fragment appended (tools/manifest/sourcefile.py: `test_url + variant`), and the file reads it back off
   `location.search` to decide which subtests to run. Running the bare URL instead runs a test the corpus does
   not itself run — an excluded test wearing a counted name — so the driver names the variant and it lands
   here, in the two addresses everything else is derived from: what to GET, and what `location` answers. */
static char   g_variant[256];   /* "" or the "?…"/"#…" this run is */

/* ONE GET against the corpus server, resolved against the running document's address. */
/* `pcsp`, when given, receives the response's `Content-Security-Policy` as a malloc'd string (NULL when the
   response carried none). The header list was already parsed here and thrown away — which is how a corpus test
   that declares a policy was measured against no policy at all. */
static char *wpt_get_csp(const char *url, size_t *plen, char **pcsp)
{
    FetchRequest req;
    HeaderList h;
    int status = 0;
    char *body;

    memset(&req, 0, sizeof req);
    memset(&h, 0, sizeof h);
    req.method = "GET";
    req.url = url;
    body = wpt_http(&req, plen, &status, &h);
    if (pcsp) *pcsp = header_list_get(&h, "content-security-policy");   /* malloc'd or NULL */
    header_list_free(&h);
    if (body && (status < 200 || status >= 300)) { free(body); body = NULL; }
    return body;
}

static char *wpt_get(const char *url, size_t *plen)
{
    return wpt_get_csp(url, plen, NULL);
}

static void wpt_derive_addresses(int argc, char **argv)
{
    const char *h = argv[1], *tail = strstr(h, "/resources/testharness.js");
    const char *test = argv[argc - 1];
    size_t n = tail ? (size_t)(tail - h) : 0;

    /* THE CORPUS ROOT IS DERIVED FROM THE HARNESS PATH, so a first argument that is not the corpus's
       testharness.js leaves it EMPTY — and every address below then carries the runner's own filesystem path
       into a URL, which the server answers with a 404 nobody can read backwards. */
    CHECK(tail != NULL, "wpt: argv[1] is not the corpus's resources/testharness.js, so the corpus root is unknown");
    CHECK(n < sizeof g_wpt_root, "wpt: the corpus root is longer than this runner holds");
    memcpy(g_wpt_root, h, n);
    g_wpt_root[n] = 0;
    /* THE LAST argument is the test; the ones between are its META scripts. Reading argv[2] made a file with a
       META script resolve its data against that script's directory instead of its own.
       ONE ADDRESS, BOTH KINDS. A document test and a script test each live at their own path in the corpus, so
       each has the same one address — what to GET, and what a relative URL resolves against. This was two
       branches computing the same string, of which only the document one also kept it; the script branch threw
       it away and the test was then read off disk, which is how a `.sub.` script test came to run with its
       `{{host}}` placeholders intact. */
    CHECK(!strncmp(test, g_wpt_root, n), "wpt: a test outside the corpus root has no server address");
    snprintf(g_test_url, sizeof g_test_url, "http://web-platform.test%s%s", test + n, g_variant);
    snprintf(g_base_url, sizeof g_base_url, "%s", g_test_url);
    {
        /* WHERE THE CORPUS IS SERVED FROM. The driver starts wptserve and names its loopback port here; the
           runner speaks HTTP to it and sends the URL's own authority as the Host header, which is what a
           hosts-file mapping does for a real browser. A run with no server has nothing to fetch from, and that
           is a GATE defect rather than a result, so it fails loud. */
        const char *sv = getenv("WPT_SERVER");
        CHECK(sv && *sv && strchr(sv, ':'),
              "wpt: WPT_SERVER names no host:port — the driver must start engine/wptserve.py and pass it");
        CHECK(strlen(sv) < sizeof g_server, "wpt: WPT_SERVER is longer than this runner holds");
        snprintf(g_server, sizeof g_server, "%s", sv);
    }
}

/* Run one program as a FLOW, exactly as the test262 harness does: park and rebuild the whole frame chain at
   every back-edge. */
/* THE TOP-LEVEL DOCUMENT'S PROGRAM DRIVER, AND THE ONE DRIVE-TO-COMPLETION LEFT IN THIS FILE. It is not what a
 * program is in this engine — a program is a work item on the ONE frontier, ranked, preemptible and parkable at
 * any depth — and every yield `JS_FlowResume` reports here is handed straight back to the same program by the
 * next iteration, so the flow machinery's park-and-rebuild runs while the SCHEDULER never does.
 *
 * THE CHILD DOCUMENT NO LONGER USES IT: wpt_child_main runs its document's scripts, its deliveries and the
 * programs that answer a peer's operations through engine_sched_begin/engine_sched_step, which is the same
 * scheduler main.c and test_forced.c drive. Two things stand between THIS half and that, and neither is a
 * decision — both are capabilities the scheduler does not have yet, and each is checkable in one line:
 *   - the session's program sequence is compiled in ONE realm (flow_step's JS_FlowNew takes the session's
 *     ctx), and this host queues programs belonging to a CHILD REALM's document (wpt_queue_program), which has
 *     no way onto the frontier;
 *   - the host's reply seam names a URL and nothing else (engine_pending_urls / engine_provide), and the
 *     corpus asks this host for POSTs whose answer depends on the BODY it sent — so a fetch parked on the
 *     frontier could not be answered correctly, which is why this half still owes its replies through g_owed.
 * A `while (JS_FlowResume)` per program is what the absence of those two costs, and it is stated here rather
 * than left to be rediscovered from the shape of the loop. */
static int run_program(JSContext *ctx, const char *src, size_t len, const char *name)
{
    JSValue *flow;
    JSValue res = JS_UNDEFINED;

    flow = JS_FlowNew(ctx, src, len, name, JS_EVAL_TYPE_GLOBAL);
    if (!flow) {
        wpt_report_exception(ctx, name, "");
        return 1;
    }
    while (JS_FlowResume(ctx, flow, &res)) { wpt_answer_host_requests(ctx); }
    if (JS_IsException(res)) {
        wpt_report_exception(ctx, name, "");
        JS_FlowFree(ctx, flow);
        return 1;
    }
    JS_FreeValue(ctx, res);
    JS_FlowFree(ctx, flow);
    return 0;
}

static lxb_html_document_t *g_wpt_dom;   /* the runner's parsed document */

/* EVERYTHING A DOCUMENT IS, built once — and built by ONE function because there is more than one kind of
 * document now. A TEST document and a CHILD document (the navigable an <iframe> or a popup created, which
 * SECURITY.md puts in an instance of its own) differ in exactly three things: the NAME the world registry
 * knows them by, their ORIGIN, and which bytes their tree is parsed from. Everything else — the platform
 * surface, the browsing context, the prototype chain, the event loop — is the same document machinery, and
 * two copies of it would be two browsers that drift.
 *
 * `html`/`html_n` is the document to parse; NULL means the minimal wrapper WPT puts around a .window.js.
 * `g_base_url` and `g_server` must already name where this document lives — see wpt_derive_addresses. */
static JSRuntime *g_rt;

/* THIS BUILD'S AGENT AND THIS BUILD'S DOCUMENT, SPLIT — because they are two different lifetimes and were one
 * function. An AGENT is a JSRuntime: the class registry every component registers into, the world registry that
 * names documents, the flow frontier. A DOCUMENT is a JSContext in it: a global with the platform installed on
 * it, a parsed tree, an address. A SAME-ORIGIN CHILD NAVIGABLE IS A SECOND DOCUMENT IN THIS AGENT, so "what a
 * document is" has to be one description that can run twice — while it was one function, a second document
 * meant a second PROCESS and there was no third option to reach for.
 *
 * AND WHAT EACH HALF *IS* IS NOT WRITTEN HERE — core/platform.h holds the one list, because three hosts each
 * writing out their own copy is the same defect one level up from the one core/realm.h fixed, and it had
 * already cost this gate four standards. What stays in this function is this runner's own: the solver
 * bootstrap it needs to exercise real components, and its network edge. */
static void wpt_agent_init(JSContext *ctx, const char *doc_name, const char *origin,
                           const char *top_level_url)
{
    PlatformAgent agent;

    /* THE COMPONENTS UNDER TEST ARE NOT NAMED HERE ANY MORE, and that is the fix rather than a convenience.
       This list used to be typed out one component at a time, "because a component that is not installed
       makes its tests fail LOUDLY on a missing global — which is the honest report". It is not the honest
       report: a component missing from THIS copy of the list is missing from the GATE, and the gate then
       publishes a number for the area as though it had run it. Five were missing — navigator, screen,
       storage_manager, unhandled_rejection, module_loader — and so were §8.1.7.2's Window event handlers, so
       every unqualified `onload = …` in the corpus wrote a property nothing fires. `navigator` is the
       expensive one: the-navigator-object was collected and failing on a missing global, and Permissions
       §6.2, Storage §3, the File System Access surface `navigator.storage` reaches and HTML §6.4.4's
       UserActivation had no door into this runner at all. There is ONE list now (core/platform.h) and this
       host cannot express an omission — it can only add. */

    /* NAME THIS DOCUMENT. A WindowProxy answers "is this navigable remote?" by comparing against the one
       document identity the world registry owns, so the registry has to be up before the first proxy exists. */
    world_registry_init(doc_name);
    /* THE CONCOLIC VALUE CLASS. This runner exercises the REAL components, and several of them answer with a
       concolic value where the spec's answer is genuinely unknown input — `window.name` survives navigation, so
       an attacker who can open the document sets it. The class has to be registered before any component can
       mint one; without it every file here aborted at the first such read, and location.c's two attacker
       SOURCES are declared out of it. */
    concolic_init(ctx);
    /* THE VALUE SEMANTICS, WITHOUT THE ABSENT-GLOBAL SOURCE. A conformance run reaches concolic values — the
       two Location sources are the document's own address — and every operator over one needs the hooks or it
       throws at the first coercion. What it must NOT have is a global that was never set becoming unknown
       input: the spec answers that with a ReferenceError, and the corpus tests exactly that. */
    concolic_install_hooks();
    /* THE SOLVER FRONTIER, because a member that SUSPENDS parks on a flow's pending register and that register
       is the frontier's. WHAT IS IN IT IS THE SCHEDULER'S — engine_sched_begin seeds it and asserts that it
       finds it empty, so an agent init that put a flow there could never be the agent of a session. This one
       did: it added ONE flow and left it switched in for the whole run, which is what let every drive-to-
       completion loop in this file exist without anything crashing — flow_running() answered a flow that ran
       nothing and parked never. A host that fabricates the scheduler's state is a second scheduler with the
       loop left out. */
    flow_registry_init(doc_name);
    /* THE DOM CHOKEPOINT'S CONTEXT. §4.2.3's insertion and removing steps are fired from the solver's tree
       chokepoint, which needs the runtime they run in — and this runner never named one, so it ran NONE of
       them: no <script> preparation, no custom-element upgrade, no §4.8.5 child navigable. It failed
       silently, three layers away, as an iframe whose contentWindow was null. */
    dom_cow_set_ctx(ctx);
    /* THIS HOST'S NETWORK EDGE — WHO answers, which is the one thing that is legitimately this runner's and
       not the platform's: it serves the checked-out corpus over wptserve's own socket. An edge is a
       PARAMETER, and fetch.c aborts on a fetch issued with none, which is what asserts this line is here. */
    { static const FetchProvider P = { wpt_owe }; fetch_set_provider(&P); }

    agent.origin = origin;
    agent.top_level_url = top_level_url;
    platform_agent_init(ctx, &agent);
}

/* ONE DOCUMENT. Runs once per document INCLUDING the first, which is what makes it the one description of what
   a document of this build is — a same-origin child navigable gets exactly this and nothing else. */
static void wpt_realm_install(JSContext *ctx, lxb_html_document_t *dom, const char *url, const char *origin,
                              const char *csp, uint32_t doc_id, JSValueConst nav_proxy)
{
    PlatformDocument doc;
    JSValue global = JS_GetGlobalObject(ctx);

    /* `self` IS NOT SET HERE. It is §7.2.2's [Replaceable] Window member and the platform's window_install
       installs it as one; a plain own value written first was a STAND-IN for the member under test, and a
       stand-in is how a gate comes to agree with itself — this one would have hidden the member being absent
       entirely. */
    JS_SetPropertyStr(ctx, global, "print", JS_NewCFunction(ctx, js_wpt_print, "print", 1));
    /* `gc` — what /common/gc.js reaches for first, and the only way a test that asserts a stream survives
       COLLECTION can assert anything at all. It is the runner's, never the browser's: a page-visible collector
       is fingerprintable and this engine does not ship one. quickjs's own JS_RunGC is the whole of it. */
    JS_SetPropertyStr(ctx, global, "gc", JS_NewCFunction(ctx, js_wpt_gc, "gc", 0));

    /* AND THEN THE PLATFORM — every component, in the one order, from core/platform.h. The two lines above are
       the runner's own and are ADDED to it; there is nothing here that can subtract from it.
       §4.8.5 IS WHY `document` COMES LAST IN THAT LIST rather than in this file's order: installing it runs
       the insertion steps for every <iframe> in the markup, which CREATES a child navigable and therefore
       needs the WindowProxy class and §7.4's create-a-navigable to exist first. This runner had it backwards
       and the assert in window_proxy_new_remote said so the moment a parsed iframe reached it.
       THE ADDRESS, NOT THE ORIGIN, is what a Window is installed at — this host passed `origin` where the
       other two passed the address, which is the substitution a two-field record makes unspellable. */
    doc.dom = dom;
    doc.url = url;
    doc.origin = origin;
    doc.csp = csp;
    doc.doc_id = doc_id;
    doc.nav_proxy = nav_proxy;
    platform_document_install(ctx, global, &doc);

    JS_FreeValue(ctx, global);
}

/* A SAME-ORIGIN CHILD NAVIGABLE'S REALM — a second JSContext in the SAME JSRuntime, which is what HTML's
   similar-origin window agent is. It gets the identical per-document install the first document got; there is
   no smaller variant of it, because a child whose `window` is smaller is a different browser. */
/* PROGRAMS A DOCUMENT OWES, waiting for the pump. A child realm is built inside the creator's `window.open()`,
   so its scripts cannot run there — see the queue's one consumer in main. Each entry owns its bytes and its
   name; the realm is BORROWED, because the agent owns every realm it built and outlives this queue. */
typedef struct { JSContext *ctx; char *src; size_t len; char *name; } PendingProgram;
static PendingProgram *g_pending;
static int             g_pending_n, g_pending_cap;

static void wpt_queue_program(JSContext *ctx, const char *src, size_t len, const char *name)
{
    PendingProgram *e;

    if (g_pending_n == g_pending_cap) {
        int cap = g_pending_cap ? g_pending_cap * 2 : 8;
        PendingProgram *g = realloc(g_pending, (size_t)cap * sizeof *g);
        CHECK(g != NULL, "wpt: OOM queuing a document's program — a dropped program is a document that never ran");
        g_pending = g;
        g_pending_cap = cap;
    }
    e = &g_pending[g_pending_n++];
    e->ctx = ctx;
    e->src = malloc(len + 1);
    CHECK(e->src != NULL, "wpt: OOM copying a queued program");
    memcpy(e->src, src, len);
    e->src[len] = 0;
    e->len = len;
    e->name = strdup(name ? name : "<document>");
    CHECK(e->name != NULL, "wpt: OOM naming a queued program");
}

/* Run one queued program, or report that there were none. Called from the pump, which is the only thing that
   may enter JS: this is a program starting, exactly like the test file's own. */
static bool wpt_run_pending(void)
{
    PendingProgram e;

    if (g_pending_n == 0) return false;
    e = g_pending[0];
    memmove(g_pending, g_pending + 1, (size_t)(--g_pending_n) * sizeof *g_pending);
    run_program(e.ctx, e.src, e.len, e.name);
    free(e.src);
    free(e.name);
    return true;
}

static JSContext *wpt_child_realm(JSRuntime *rt, lxb_html_document_t *dom, const char *url,
                                  const char *top_level_url, const char *origin, const char *csp,
                                  uint32_t doc_id, JSValueConst nav_proxy)
{
    JSContext *ctx = JS_NewContext(rt);

    CHECK(ctx != NULL, "wpt: a same-origin child navigable's realm could not be created");
    /* §3.7: a realm gets its OWN intrinsics — the members on them run in the realm that DEFINED them, so a
       child sharing the agent realm's EventTarget.prototype would resolve every unqualified
       `addEventListener` against the PARENT's window. They come from the ONE list the components declared
       themselves into, so a component added anywhere is installed in every realm with no host to edit.
       §7.4 decided the CHILD's top-level creation URL and handed it over; a builder that used `url` here
       would make an about:blank iframe of an http page a secure context. */
    realm_install_intrinsics(ctx, top_level_url);
    wpt_realm_install(ctx, dom, url, origin, csp, doc_id, nav_proxy);
    /* THE CHILD'S SCRIPTS ARE THE CHILD'S, run in ITS realm — they are what make a popup a participant rather
       than an empty frame, since message-opener.html's whole body is one script that posts to its opener.
       THEY ARE QUEUED, NOT RUN HERE. A realm is built from inside the creator's `window.open()` — a C
       activation in the middle of the parent's own program — so running the child's scripts at this point is a
       nested drive-to-completion re-entering JS from C: exactly the shape this engine refuses everywhere else,
       and it surfaced as `JS_ToPrimitiveFree reached with a real object` the moment a popup's script coerced
       anything. A document's scripts are PROGRAMS, and a program is a flow the one pump drives. Queuing them
       is what makes the child's script ordinary work on the same loop as the parent's, which is also what lets
       one of them suspend. */
    {
        DocScripts ds = document_exec_scripts(dom);
        int i;
        for (i = 0; i < ds.n; i++) {
            if (ds.bodies[i]) wpt_queue_program(ctx, ds.bodies[i], strlen(ds.bodies[i]), url);
            else if (ds.srcs[i]) {
                size_t sl = 0;
                char *body = wpt_get(ds.srcs[i], &sl);
                if (!body) continue;
                wpt_queue_program(ctx, body, sl, ds.srcs[i]);
                free(body);
            }
        }
        doc_scripts_free(&ds);
    }
    return ctx;
}

/* THE FIRST DOCUMENT, which is also what brings the agent up. A REAL LEXBOR PARSE — the same one the engine
   runs — of the test's own bytes when the test is an HTML file, and otherwise of the minimal document WPT's
   server wraps a `.window.js` in. One parse either way; the only difference is whose bytes. */
static JSContext *wpt_build_document(const char *doc_name, const char *origin, const char *top_level_url,
                                     const char *html, size_t html_n)
{
    static const char DOC[] = "<!doctype html><html><head></head><body></body></html>";
    char *fetched = NULL, *root_csp = NULL;   /* the response's policy — §7.2.6's header half */
    const char *src = html;
    JSRuntime *rt;
    JSContext *ctx;

    rt = JS_NewRuntime();
    JS_SetMaxStackSize(rt, 4 * 1024 * 1024);
    ctx = JS_NewContext(rt);
    wpt_agent_init(ctx, doc_name, origin, top_level_url);
    /* §7.4 CALLS BACK HERE FOR A SAME-ORIGIN CHILD, because what a document of this build IS is this runner's
       answer and not the engine's — a child with a different platform surface would make every fidelity number
       measured in it a number about a different browser. */
    navigable_set_realm_builder(wpt_child_realm);

    if (!src) {
        src = DOC;
        html_n = sizeof DOC - 1;
        if (g_html_mode) {
            /* THE POLICY COMES WITH THE BYTES. A corpus file served with a `.headers` sidecar declares its
               Content-Security-Policy there, and reading only the body measured every such test against no
               policy — the same half-a-container defect the product host had. */
            fetched = wpt_get_csp(g_test_url, &html_n, &root_csp);
            CHECK(fetched != NULL, "wpt: the corpus server did not serve the HTML test file");
            src = fetched;
        }
    }
    g_wpt_dom = lxb_html_document_create();
    CHECK(g_wpt_dom != NULL, "the runner's document allocation failed");
    CHECK(lxb_html_document_parse(g_wpt_dom, (const lxb_char_t *)src, html_n) == LXB_STATUS_OK,
          "the runner's document did not parse");
    free(fetched);

    /* THE ROOT NAVIGABLE IS THE HOST'S, so its §7.2.5.1 proxy is minted here — the same rule as every child,
       whose creator mints it. A navigable has one, and whoever owns the navigable is who makes it. */
    {
        /* THE RUNNER LOADED THIS DOCUMENT ITSELF, so it knows the navigable's name is the spec's initial "" —
           nothing opened it under a name. Saying so is what keeps `window.name` a computed value here. */
        JSValue root_proxy = window_proxy_new_self(ctx, world_local_doc(), "");
        CHECK(!JS_IsException(root_proxy), "the root navigable's WindowProxy could not be allocated");
        wpt_realm_install(ctx, g_wpt_dom, g_base_url, origin, root_csp, world_local_doc(), root_proxy);
        JS_FreeValue(ctx, root_proxy);
    }
    free(root_csp);   /* document_install built its container from it and keeps no pointer */
    /* SEALED AFTER THE FIRST DOCUMENT, and only that one: a component DECLARES its IDL members in its init and
       INSTALLS from the cached id, so a second realm's install declares nothing. A declaration reached from the
       second install is therefore a component that mints per-global, and the seal says so at the first one. */
    idl_args_seal();
    g_rt = rt;
    return ctx;
}

/* THE CHILD DOCUMENT'S OWN MAIN. It is a document, not a test: it parses its address's bytes, runs that
 * document's scripts, and then ANSWERS — it prints no results, because it has none.
 *
 * IT ANSWERS BY RUNNING A PROGRAM, not by reading a property from C. `self.length` is an IDL getter and the
 * engine refuses to reach one from a C activation: there is no flow base under it, so a loop in the getter's
 * body would drive to completion instead of parking. The read is a flow like every other read, which is also
 * what makes an answer that has to suspend possible at all.
 *
 * THE WORLD ARRIVES WITH ITS ANCESTRY and both are used: this loop installs the asking flow's segment before
 * it answers, materializing it by forking the nearest ancestor this instance already holds. That matters for a
 * READ and is what makes a WRITE possible at all — two arms of one fork writing through one reference are two
 * timelines, and a peer that performed both against one baseline would build a third neither arm was in. */
/* WHAT AN OPERATION IS — its verb, its field layout, its operands and the program that performs it — is NOT
   here any more, and that is the point of the file it moved to. It was written out in this serve loop, where
   the production ABI entry could not reach it, so the shipped engine had a sender with no receiver; a second
   copy written for that entry would have been two spellings of one grammar. core/frame/remote_op.h.
   AND NEITHER IS THE WORLD SWITCH, WHICH WAS THE REST OF THE SECOND SCHEDULER. This loop used to unapply the
   last answer's segment and apply the new one itself, "because this serve loop IS this instance's scheduler" —
   which is exactly the sentence that had to stop being true. engine_route and engine_perform materialize the
   asking world's segment where they attach the record, flow_deliver and flow_perform ask again at the moment
   the work actually runs, and the switch between timelines is flow_switch_in/out's, once, for every kind of
   work. What is left here is this host's own: the CHANNEL it speaks (one line in, one line out) and the
   ROUTING of what arrives on it. */

/* A CHILD CANNOT INITIATE, so its notices ride back on the answer pipe prefixed `N\t` and the parent's router
   takes them from there. Drained after every step, because a notice held is a delivery its target instance has
   not been told about — and because one of them is not a notice at all: `remoteop.answer` is the COMPLETION of
   the operation this channel asked for, so it leaves as the answer LINE. It is recognised by its rendezvous
   TOKEN and not by being the only one of its kind: a document's state is its flows, so a document with N
   timelines answers N times, and a second answer arriving for one question is a real thing this channel cannot
   yet carry rather than something to overwrite the first with. */
static char *g_child_answer;   /* the completion for the token in flight, owned here until it is written */

static void wpt_child_emit_notices(const char *token)
{
    const char *notices = engine_host_notices();
    const char *p;

    for (p = notices; *p; ) {
        const char *end = strchr(p, '\n');
        size_t n;
        if (!end) break;
        n = (size_t)(end - p);
        if (token && !strncmp(p, "remoteop.answer\t", 16)) {
            const char *tok = p + 16;
            const char *tab = memchr(tok, '\t', n - 16);
            DCHECK(tab != NULL, "a cross-agent operation's answer carried no completion after its token — the "
                                "channel would relay a rendezvous with nothing in it and the asking flow would "
                                "park on a question it has already been answered");
            if (tab && (size_t)(tab - tok) == strlen(token) && !memcmp(tok, token, strlen(token))) {
                DCHECK(g_child_answer == NULL,
                       "a second completion arrived for one cross-agent operation — this document has more than "
                       "one timeline and every one of their answers is true, so the channel that asked has to "
                       "be able to carry N of them: build the multi-answer reply before a peer may fork here");
                g_child_answer = strndup(tab + 1, n - (size_t)(tab + 1 - p));
                CHECK(g_child_answer != NULL, "wpt: OOM keeping a cross-agent operation's completion");
                p = end + 1;
                continue;
            }
        }
        fputs("N\t", stdout);
        fwrite(p, 1, n, stdout);
        fputc('\n', stdout);
        p = end + 1;
    }
}

/* THIS HOST'S LOOP OVER THE ONE SCHEDULER — a host with nothing else to do between quanta, which is what
   engine.h says a driver like this is: the state machine is unchanged and a quantum boundary is invisible to
   it. It is not a second scheduler and it is not a drive-to-completion: every program this instance runs (its
   document's scripts, a delivery's task, the program that performs a peer's operation) is a work item on the
   ONE frontier, preemptible and parkable at any depth, and this only hands the thread over and takes it back.
   IT ENDS AT THE STALL AND NEVER AT DONE. A document a peer holds a reference into may not run out of
   timelines (engine.h's engine_set_referenced), so its frontier is never exhausted — it stalls, waiting for
   the next thing to arrive on this channel, which is precisely where the thread belongs. */
static void wpt_child_run(const char *token)
{
    for (;;) {
        int r = engine_sched_step();
        wpt_child_emit_notices(token);
        if (r == ENGINE_STEP_YIELD) continue;
        DCHECK(r == ENGINE_STEP_STALLED,
               "the scheduler declared this instance's frontier EXHAUSTED — a document another agent holds a "
               "reference into has no such state, so either engine_set_referenced was not set for it or the "
               "session was closed while a peer could still ask it something");
        return;
    }
}

static int wpt_child_main(int argc, char **argv)
{
    const char *name = argv[2], *url = argv[3], *origin = argv[4], *csp = argc > 5 ? argv[5] : "";
    /* HTML §8.1.3.1's TOP-LEVEL CREATION URL, from the parent process that created this navigable. NOT
       defaulted to this document's own address: this instance's document may be NESTED, and answering from
       its own address would make an https child of an http page report itself a secure context — which is the
       ancestral hole Secure Contexts §4.2 exists to close, so a missing one is a parent that has not decided
       what it created rather than a value to guess. */
    const char *top_level_url = argc > 6 ? argv[6] : NULL;
    JSContext *ctx;
    char *html = NULL;
    size_t html_n = 0;
    char line[65536];   /* a routed message rides this channel base64'd — see windowproxy.post below */
    /* THE SESSION'S PROGRAM SEQUENCE, which outlives every statement in this function: the scheduler BORROWS
       these arrays for the life of the session (engine_sched_begin), so a document's scripts may not be a
       block-scoped temporary the way they were when this host ran them itself. */
    DocScripts ds;

    (void)csp;
    /* THE ANSWER PIPE IS STDOUT HERE, so nothing else may write to it — see report_out. */
    g_report = stderr;
    setvbuf(stdout, NULL, _IOLBF, 0);
    {
        const char *sv = getenv("WPT_SERVER");
        CHECK(sv && *sv, "wpt: a child document was started with no corpus server to load its address from");
        snprintf(g_server, sizeof g_server, "%s", sv);
    }
    CHECK(top_level_url != NULL && *top_level_url,
          "wpt: a child document was started with no TOP-LEVEL CREATION URL — HTML §8.1.3.5 reads it to decide "
          "whether this document's realm is a secure context, and Web IDL §3.3.13's members exist or do not by "
          "that answer, so the platform surface of this instance would be a guess");
    snprintf(g_base_url, sizeof g_base_url, "%s", url && *url ? url : "about:blank");
    /* `about:blank` HAS NO BYTES TO FETCH — its Document is the empty one §7.4 creates, which is what makes it
       synchronous in a browser and what makes an <iframe> with no src scriptable immediately. */
    if (strncmp(g_base_url, "about:", 6) != 0) {
        html = wpt_get(g_base_url, &html_n);
        /* A child whose address does not load still HAS a document — a browser shows an error page and the
           navigable exists — so this is about:blank rather than a dead instance. */
        if (!html) html_n = 0;
    }
    /* THE CHILD PROCESS'S TOP-LEVEL CREATION URL came from the parent with the rest of the environment —
       this instance's document may be NESTED in a document of another instance, and only the zone that
       created it knows which. A cross-origin child that answered from its OWN address would report itself a
       secure context inside an insecurely-delivered page, which is exactly the ancestral hole Secure
       Contexts §4.2 exists to close. */
    ctx = wpt_build_document(name, origin, top_level_url, html, html_n);
    free(html);

    /* THIS INSTANCE EXISTS BECAUSE A PEER CREATED ITS NAVIGABLE and still holds a WindowProxy for it, so its
       timelines may not run out — engine.h's engine_set_referenced, and the reason this line comes before the
       session rather than after it. Without it the one flow below finishes with its last script, the frontier
       drains, the session closes, and the first thing a peer asks arrives at a document with no timeline to
       answer in. Stated by the HOST because provisioning is the host's fact: the engine cannot see its own
       embedder. */
    engine_set_referenced(1);

    /* THE TWO INTRINSICS A PEER PERFORMS A WRITE AND A CALL THROUGH are captured by the component that performs
       them, at this realm's install — before this document's scripts run, which is the whole of what makes them
       intrinsics rather than whatever the page has left on `Reflect`. There is nothing for this host to do. */

    /* THIS DOCUMENT'S SCRIPTS ARE THE SESSION'S PROGRAM SEQUENCE, and this host no longer runs them: it hands
       them to the ONE scheduler and steps it. What that deletes is a `while (JS_FlowResume)` per script — a
       drive-to-completion with the frontier bypassed, which is the one thing §scheduler forbids at every depth
       and which this host had at the very top of every program it ran.
       AN EXTERNAL SCRIPT IS FETCHED INTO ITS OWN SLOT, in position, because classic scripts run in document
       order and one that did not load is a script that runs nothing rather than one that moves the others up.
       (The engine's own answer for an external script is the docscript park — the flow waits and the host
       fills the shared slot — and this host will use it the moment its network edge can carry a REQUEST rather
       than a URL; today's `fetch` seam names only the URL, which cannot say which POST body a corpus handler
       is being asked to echo.) */
    ds = document_exec_scripts(g_wpt_dom);
    {
        int i;
        for (i = 0; i < ds.n; i++) {
            if (ds.bodies[i] || !ds.srcs[i]) continue;
            {
                size_t len = 0;
                char *body = wpt_get(ds.srcs[i], &len);
                ds.bodies[i] = body ? body : strdup("");
                CHECK(ds.bodies[i] != NULL, "wpt: OOM loading a child document's external script");
            }
        }
    }
    engine_sched_begin(ctx, ds.bodies, ds.srcs, ds.n, /*forking*/0, NULL);
    /* THE DOCUMENT RUNS BEFORE THE FIRST QUESTION ARRIVES, which is what makes a child a participant rather
       than an empty frame — message-opener.html's whole body is one script that posts to its opener, and that
       post has to be on this channel before the parent asks anything. */
    wpt_child_run(NULL);

    while (fgets(line, sizeof line, stdin)) {
        char *nl = strchr(line, '\n');
        /* A RECORD THAT DID NOT FIT arrived as two, and the tail would then be read as an operation of its own.
           The channel is line-oriented, so the only safe answer to a truncated read is to stop. */
        CHECK(nl != NULL || feof(stdin), "wpt: a cross-instance record was longer than this child's line buffer");
        char *answer;
        /* THE CHANNEL'S TWO SHAPES, and only the first field is this host's — everything after it is the
           EMITTING ENGINE'S RECORD, verbatim, handed to the entry that reads it. The zone contributes exactly
           the two things a record may not carry itself: the SENDER'S ORIGIN, which only the trusted zone may
           state (SECURITY.md), and the RENDEZVOUS TOKEN, which is the zone's because a request id is unique
           only inside the instance that minted it.
             `post <sender origin> <record>`  — §9.4.4 step 7 arriving. Nothing is asked; a delivery is MADE.
             `op <token> <record>`            — a cross-agent operation this document is asked to perform. */
        int is_post = !strncmp(line, "post\t", 5);
        int is_op   = !strncmp(line, "op\t", 3);
        char *f1, *rec;

        if (nl) *nl = 0;
        CHECK(is_post || is_op, "wpt: a record arrived on a child's channel under a verb it does not carry");
        f1 = line + (is_post ? 5 : 3);
        rec = strchr(f1, '\t');
        CHECK(rec != NULL, "wpt: a record arrived on a child's channel with no engine record after the "
                           "transport's own field");
        *rec++ = 0;
        DCHECK(g_child_answer == NULL,
               "a completion for the previous question was never written to this channel — the parent is "
               "blocked on a line that is not coming");
        if (is_post) {
            /* NOTHING RUNS INSIDE THIS CALL. The record becomes a work item of every live timeline of this
               document, and each of them makes its own delivery when the scheduler next runs it, under its own
               delta — which is the whole reason the delivery is attached rather than performed: the page's
               `message` listener was registered by a script, so it lives in the delta of the flow that ran that
               script and nowhere else. This host used to take the record apart itself, route the payload, and
               then run an EMPTY PROGRAM to completion as a way of turning the task queue over. */
            engine_route(ctx, rec, f1);
            wpt_child_run(NULL);
            /* THE ACK IS THE TRANSPORT'S, not a program's completion: nothing was asked, so there is nothing
               for a timeline to answer. It is written through the one encoder so this pipe has ONE grammar
               rather than one for the answers and a bare `u` for this. */
            answer = remote_completion_encode(ctx, ENGINE_COMPLETION_NORMAL, JS_UNDEFINED);
        } else {
            /* A PEER ANSWERS BY RUNNING A PROGRAM, and the program is a FLOW on this instance's frontier — so
               an answer that has to suspend can exist at all, and every other timeline of this document keeps
               running while it does. What this host does is hand the operation over and step the scheduler
               until it has nothing runnable; the completion arrives the way every other emission does, as a
               notice naming the token, and wpt_child_emit_notices takes it off the stream.
               THE THROW IS THE ANSWER and needs nothing here: flow_answer_perform encodes the completion with
               its TYPE, so a peer's `try`/`catch` runs at the line that asked. */
            engine_perform(ctx, f1, rec);
            wpt_child_run(f1);
            DCHECK(g_child_answer != NULL,
                   "this instance stalled without answering the operation it was asked — every timeline it was "
                   "attached to either finished without completing its program or is parked on something this "
                   "host owes and does not know it owes; the asking flow is suspended at the line that asked");
            answer = g_child_answer;
            g_child_answer = NULL;
        }
        CHECK(answer != NULL, "wpt: OOM writing a completion to this child's channel");
        fputs(answer, stdout);
        fputc('\n', stdout);
        fflush(stdout);
        free(answer);
    }
    return 0;
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
    int i, failed = 0;

    g_self_exe = argv[0];
    /* A CHILD NAVIGABLE'S INSTANCE, re-executed from this same binary — see the child-document section. */
    if (argc >= 5 && !strcmp(argv[1], "--document")) return wpt_child_main(argc, argv);
    /* THE DRIVER'S CLASSIFICATION, consumed before anything else reads argv. Everything downstream then sees
       the same argument shape it always saw, so there is no second indexing rule to keep in step. */
    while (argc >= 2 && argv[1][0] == '-' && argv[1][1] == '-') {
        if (!strcmp(argv[1], "--test-document")) { g_html_mode = 1; argv++; argc--; }
        else if (!strcmp(argv[1], "--variant")) {
            CHECK(argc >= 3, "wpt: --variant names no variant");
            /* THE CORPUS'S OWN RULE, asserted where the value arrives rather than trusted from the driver:
               sourcefile.py rejects a non-empty variant that does not begin with `?` or `#`, because a variant
               is a query or a fragment and nothing else. A variant that is neither would silently become part
               of the PATH here and this runner would GET a file that does not exist. */
            CHECK(argv[2][0] == '?' || argv[2][0] == '#',
                  "wpt: a variant must begin with ? or # — it is a query or a fragment, not a path");
            CHECK(strlen(argv[2]) < sizeof g_variant, "wpt: the variant is longer than this runner holds");
            snprintf(g_variant, sizeof g_variant, "%s", argv[2]);
            argv += 2; argc -= 2;
        } else break;
    }
    /* THE HARNESS AND THE TEST ARE BOTH REQUIRED, which the usage line has always said and the check has not:
       it accepted `argc >= 2`, and with only the harness given every rule below that says "the LAST argument is
       the test" would have named the harness as the test. */
    if (argc < 3) {
        fprintf(stderr, "usage: wpt_runner [--test-document] [--variant ?v] <testharness.js> <test> [more.js ...]\n");
        return 2;
    }
    /* WHICH FILE IS THE TEST, WHERE IT LIVES, AND WHETHER IT IS A DOCUMENT. Derived before anything else is
       built, because an HTML test's document has to be FETCHED and PARSED before `document` is installed —
       there is no second document to swap in later, and a runner that installed a placeholder first would be
       running the test's scripts against the wrong tree. */
    wpt_derive_addresses(argc, argv);
    /* THE TEST DOCUMENT. One call, because a child document is built by the same one — see above. */
    /* THE TEST DOCUMENT IS THIS PROCESS'S TOP-LEVEL TRAVERSABLE, so its environment's top-level creation
       URL is its own address — the runner navigated to it and nothing embeds it. */
    ctx = wpt_build_document("wpt", WPT_TOP_ORIGIN, g_base_url, NULL, 0);
    /* THE REALM A RELAYED NOTICE IS DELIVERED INTO. A child's notice arrives while this process is blocked
       inside wpt_child_ask, which has no ctx of its own — this names the one it routes into, and a notice
       arriving before there is one is a message with nowhere to go. */
    g_wpt_ctx = ctx;
    rt = g_rt;

    /* THE TOP-LEVEL DOCUMENT'S FLOW, FABRICATED — this half of the runner has not been converted to the one
       scheduler yet, and this line is the whole of what stands in for it: a single flow, switched in for the
       run, so that the program driver below and everything that asks flow_running() (a live fetch, a
       synchronous host request, §7.4's open) has an answer. It is a scheduler's STATE with no scheduler, and
       it cannot quietly survive one arriving: engine_sched_begin asserts the frontier is empty, so the first
       session opened over this document crashes here rather than seeding a second timeline beside it. The
       CHILD document below is on the real thing (engine_sched_begin/engine_sched_step) and does not have it. */
    flow_set_running(flow_add(ctx, JS_UNDEFINED, WORLD_NONE));

    JS_SetFlowControlHooks(&WPT_HOOKS_ON);
    if (g_html_mode) {
        /* THE DOCUMENT'S OWN SCRIPTS, IN DOCUMENT ORDER — the same component the engine loads a real page with,
           so an HTML test's `<script src=/resources/testharness.js>` is loaded the way the page asked for it
           rather than by this runner guessing which files a document needs. Each script is its own program body
           and its own flow: classic scripts do not share a top-level scope, and concatenating them would leak
           per-<script> let/const bindings between two files the page kept apart.
           NO PROLOGUE HERE. `GLOBAL` is the shim WPT's server writes around a `.any.js`, and a document that
           never asked for it must not be given one. */
        DocScripts ds = document_exec_scripts(g_wpt_dom);
        for (i = 0; i < ds.n && !failed; i++) {
            if (ds.bodies[i]) {
                failed = run_program(ctx, ds.bodies[i], strlen(ds.bodies[i]), g_test_url);
            } else if (ds.srcs[i]) {
                size_t len = 0;
                char *body = wpt_get(ds.srcs[i], &len);
                /* A <script src> THE SERVER DOES NOT HAVE is a real 404, and a real browser fires an error
                   event and carries on rather than stopping the document. Reporting it names the file, because
                   a test whose harness failed to load reports nothing at all otherwise. */
                if (!body) { fprintf(report_out(), "@WPTERR %s: <script src> did not load: %s\n", g_test_url, ds.srcs[i]); continue; }
                failed = run_program(ctx, body, len, ds.srcs[i]);
                free(body);
            }
        }
        doc_scripts_free(&ds);
    } else {
        failed = run_program(ctx, WPT_PROLOGUE, strlen(WPT_PROLOGUE), "<wpt-prologue>");
        /* THE HARNESS AND THE META SCRIPTS ARE PROGRAM INPUTS THE DRIVER RESOLVED, so they come from the paths
           it named; a `.sub.js` among them was fetched and substituted by the driver before it handed the path
           over. The TEST is not one of those — it is the run's ADDRESS — so it comes from the server. */
        for (i = 1; i < argc - 1 && !failed; i++) {
            size_t len = 0;
            char *src = read_file(argv[i], &len);
            if (!src) { fprintf(report_out(), "@WPTERR %s: cannot read\n", argv[i]); failed = 1; break; }
            failed = run_program(ctx, src, len, argv[i]);
            free(src);
        }
        /* AND THE TEST ITSELF IS FETCHED, exactly as a document test is, because the reason is the same and it
           is not a reason about markup: a `.sub.` file is a TEMPLATE, and wptserve substitutes `{{host}}` and
           `{{ports[https][0]}}` when it serves one. Read off disk,
           `dom/events/EventListener-addEventListener.sub.window.js` opens an iframe at the literal string
           `https://{{hosts[alt][]}}:{{ports[https][0]}}/…`, which is not a URL — so the file tested the
           template rather than the test, and the driver's own substitution reached only the META scripts. One
           rule, both kinds: the test comes from the corpus's own server. */
        if (!failed) {
            size_t len = 0;
            char *src = wpt_get(g_test_url, &len);
            CHECK(src != NULL, "wpt: the corpus server did not serve the test file");
            failed = run_program(ctx, src, len, g_test_url);
            free(src);
        }
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
        /* ANSWER BEFORE RESUMING. A parked flow that is waiting on this host cannot progress until it has its
           answer, so resuming it first is a busy-wait — and JS_ResumeParkedFlow reports "I resumed one", which
           is true, so the loop below never ends. */
        wpt_answer_host_requests(ctx);
        while (JS_ResumeParkedFlow(rt)) ;
        r = JS_ExecutePendingJob(rt, &c1);
        if (r > 0) continue;
        if (r < 0) { failed = 1; break; }
        /* NOTHING LEFT TO RUN IS NOT NOTHING LEFT TO DO. A promise_test parked on `fetch` has no job pending
           and no flow parked — it is waiting on the HOST, exactly as a page waits on the network. Satisfying
           the owed replies is what re-enters JS and gives the pump more to drain; only when a drain owes
           nothing new is the file really finished. */
        if (wpt_drain_owed(ctx)) continue;
        /* THE SAME SENTENCE FOR THE SYNCHRONOUS SEAM: a flow parked on a host-owed answer has no job pending
           and no reply owed, and answering it is what gives the pump more to drain. */
        if (wpt_answer_host_requests(ctx)) continue;
        /* A DOCUMENT THAT LOADED OWES ITS SCRIPTS, and this is where they start. They were queued from inside
           the creator's `window.open()`, which is a C activation and no place to enter JS from. Before the
           clock moves, because a child's script is loading, not a timer. */
        if (wpt_run_pending()) continue;
        /* THE DOCUMENTS FINISH LOADING, and this runner used to run NONE of it — so its documents were
           `readyState: "loading"` forever, `window.onload` never fired, and HTML §8.1.7.3 step 3 saw a
           RENDER-BLOCKED document on every pass: no rendering opportunity, no `pagereveal`, and no animation
           frame, in a harness whose whole job is to measure exactly those. PER DOCUMENT, so a child navigable's
           document gets its own DOMContentLoaded and its own `load` rather than living for ever in a stage a
           single counter could only ever hold for one of them. It is BEFORE the clock moves for the reason a
           child's pending script is: the parser finishing is not a timer. */
        if (document_lifecycle_step(ctx)) continue;
        /* NOTHING QUEUED AND NOTHING OWED, which is the one moment virtual time may move — see timer.h. TWO
           task sources become due at moments on that clock: §8.1.7.3's rendering task source at the next
           rendering opportunity, and §8.6's timer source at the earliest expiry. The rendering step is asked
           first because it is the one that can defer — it yields to a timer that expires before the frame. */
        if (rendering_run_opportunity(ctx)) continue;
        if (timer_run_due(ctx)) continue;
        break;
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

    /* THE PLATFORM'S OWN LIST, UNDONE — see main.c's teardown: one call, whatever this browser declared. */
    platform_agent_free();
    rendering_free(ctx);
    page_reveal_free(ctx);
    media_query_list_free(ctx);
    viewport_free();
    visual_viewport_free();
    location_free();
    session_history_free();
    history_free();
    navigation_free(ctx);              /* HTML §7.2.6 the navigation API */
    navigation_history_entry_free(ctx);
    animation_frame_free(ctx);
    timer_free(ctx);        /* §8.6's declaration; each global's map went with its realm */
    event_loop_free(ctx);   /* §8.1.7's own record — the virtual clock and the moments beside it */
    headers_free(ctx);
    response_free(ctx);
    request_free(ctx);
    url_free(ctx);
    usp_free(ctx);
    form_data_free(ctx);
    transform_stream_free(ctx);
    writable_stream_free(ctx);
    abort_free(ctx);
    observable_free(ctx);
    broadcast_channel_free(ctx);
    message_port_free(ctx);
    window_message_free(ctx);
    navigable_free(ctx);
    /* THIS HOST'S SCHEDULER GIVES THE THREAD BACK BEFORE THE FRONTIER GOES DOWN. The serve loop IS this
       instance's scheduler and it holds ONE flow running for the whole run (see flow_registry_init above), so
       nothing else ever performs the switch-out that every other scheduler performs when its session ends. A
       frontier torn down with a flow still switched in is one whose delta, DOM head and decision state are LIVE
       rather than parked, and the release frees them as if they were parked — which flow_release asserts
       against. Here the flow has neither (this runner drives quickjs flows directly), so the switch-out is
       exactly this line; it is stated rather than left to the accident that its delta happens to be empty. */
    flow_set_running(NULL);
    flow_registry_free(ctx);
    document_free(ctx);
    iframe_free(ctx);
    element_free(ctx);
    window_free(ctx);
    remote_object_free(ctx);
    window_proxy_free(ctx);
    world_registry_free(ctx);
    report_exception_free(ctx);
    event_free(ctx);
    event_target_free(ctx);
    realm_intrinsics_free();   /* the DECLARATIONS are the agent's; each realm's prototypes went with it */
    queuing_strategy_free(ctx);
    readable_stream_free(ctx);
    blob_free(ctx);
    file_picker_free();
    file_system_access_free();
    fs_handle_free();
    fs_writable_free();
    file_system_free(ctx);
    encoding_free(ctx);
    text_stream_free(ctx);
    idl_args_free(ctx);
    /* THE CHILD DOCUMENTS THIS FILE CREATED. A child is a PROCESS: closing its pipes ends its serve loop and
       waitpid reaps it. Without this each file left one zombie per <iframe> and per popup for the whole run —
       a leak the runtime's own gc walk cannot see, because the thing leaked is not in this heap. */
    wpt_children_free();
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    /* AFTER JS_FreeRuntime, and it is the one teardown line whose ORDER is part of its meaning: what
       this releases is part of a step DEFINITION, which JS_RegisterStepDef borrows and requires to
       outlive the runtime — JS_FreeRuntime's own [stepleak] report reads `def->steps` to name each
       unfinished machine by the step it rests at. */
    idl_async_iter_free();
    return failed ? 1 : 0;
}
