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
#include "core/file/file_system.h"
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
#include "core/html/html_iframe.h"
#include "core/frame/navigable.h"
#include "solver/world.h"
#include "solver/cow.h"
#include "solver/dom_cow.h"
#include "core/frame/window_message.h"
#include "core/structured_clone.h"
#include "core/events/event_target.h"
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
#include "core/frame/session_history.h"
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
/* Report a THROWN VALUE the caller already holds. Split from the pending-exception form below because a
   program's completion is now taken by its caller — a test file's throw is a failure to report, and a
   cross-agent operation's throw is its ANSWER, and only the caller knows which of the two it has. */
static void wpt_report_thrown(JSContext *ctx, const char *name, const char *what, JSValueConst e)
{
    char *owned = NULL;
    const char *msg = JS_DiagCString(ctx, e, &owned);
    fprintf(report_out(), "@WPTERR %s: %s%s\n", name, what, msg ? msg : "?");
    JS_DiagFreeCString(ctx, msg, owned);
}

static void wpt_report_exception(JSContext *ctx, const char *name, const char *what)
{
    JSValue e = JS_GetException(ctx);
    wpt_report_thrown(ctx, name, what, e);
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

/* Relay one message to whichever instance holds `doc` — a child, or this process's own document. */
static void wpt_route_post(JSContext *ctx, const char *doc, const char *world, const char *from_origin,
                           const char *target_origin, const char *b64)
{
    ChildDoc *c = wpt_child_for(doc);

    if (c) {
        size_t cap = strlen(world) + strlen(from_origin) + strlen(target_origin) + strlen(b64) + 64;
        char *op = malloc(cap);
        CHECK(op != NULL, "wpt: OOM routing a message to a child document");
        snprintf(op, cap, "windowproxy.post\t%s\t%s\t%s\t%s", world, from_origin, target_origin, b64);
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

    /* THE SPLIT STOPS AT THE LAST FIELD AND KEEPS THE REMAINDER VERBATIM, because the last field of a
       navigable.create IS a raw CSP header and HTTP allows HTAB inside one. */
    for (q = line, f[nf++] = q; *q && nf < 7; q++)
        if (*q == '\t') { *q = 0; f[nf++] = q + 1; }
    if (nf == 7 && !strcmp(f[0], "navigable.create")) {
        /* FIELD 5 IS HTML §8.1.3.1's TOP-LEVEL CREATION URL, which crosses for the same reason field 6's
           policy container does: the child's environment is decided by the operation that created it, and the
           instance that will host the child has no way to derive it. */
        wpt_spawn_child(f[1], f[3], f[4], f[6], f[5]);
        return;
    }
    /* `windowproxy.post <target doc> <world> <target origin> <base64>` — §9.4.4 across instances. THE ORIGIN
       IS STAMPED HERE, by the zone that knows who sent it: the engine may not name its own sender
       (SECURITY.md), so the notice carries none and this adds the one it knows. */
    if (nf == 5 && !strcmp(f[0], "windowproxy.post")) {
        wpt_route_post(ctx, f[1], f[2], from_origin, f[3], f[4]);
        return;
    }
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
                    const char *answer = wpt_child_ask(c, rec);
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
            reply = body ? fetch_reply_new(ctx, status, "", &rh, body, len) : JS_NULL;
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
/* Run one program as a flow and KEEP ITS COMPLETION. A member read answered on behalf of another document is a
   program like any other and must run as a flow like any other: reading `self.length` from C would reach the
   getter with no flow base under it, which is the one thing this engine refuses.
   IT HANDS BACK THE COMPLETION, NOT A RESULT AND A REPORT. `*pres` is the value the program completed with —
   its result, or the THROWN VALUE when the return is 1 — and it is owned by the caller either way. This used
   to report the throw here and answer `undefined`, which is right for a test file and WRONG for a peer
   answering a cross-agent operation: there the throw IS the answer and has to cross as one. A program that
   does not compile completes abruptly with the SyntaxError, which is the same shape and says so in its own
   message. */
static int run_program_value(JSContext *ctx, const char *src, size_t len, const char *name, JSValue *pres)
{
    JSValue *flow;
    JSValue res = JS_UNDEFINED;

    DCHECK(pres != NULL,
           "a program was run with nowhere to place its completion — a throw would be left pending on the "
           "context and surface at whatever asked the engine for anything next");
    *pres = JS_UNDEFINED;
    flow = JS_FlowNew(ctx, src, len, name, JS_EVAL_TYPE_GLOBAL);
    if (!flow) {
        *pres = JS_GetException(ctx);
        return 1;
    }
    while (JS_FlowResume(ctx, flow, &res)) { wpt_answer_host_requests(ctx); }
    if (JS_IsException(res)) {
        *pres = JS_GetException(ctx);
        JS_FlowFree(ctx, flow);
        return 1;
    }
    *pres = res;
    JS_FlowFree(ctx, flow);
    return 0;
}

/* A PROGRAM WHOSE THROW IS A FAILURE — a test file, the harness, a document's own script. That is a judgement
   about this caller's program and not a property of running one, which is why it lives here. */
static int run_program(JSContext *ctx, const char *src, size_t len, const char *name)
{
    JSValue completion = JS_UNDEFINED;
    int failed = run_program_value(ctx, src, len, name, &completion);

    if (failed) wpt_report_thrown(ctx, name, "", completion);
    JS_FreeValue(ctx, completion);
    return failed;
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
 * WHERE THE NEXT GAP IS, stated because the split is what makes it visible rather than because it is excused: a
 * component that builds its PROTOTYPE in its _init builds it in whichever realm was current when the agent came
 * up, and a second realm then SHARES it — where a browser gives each realm its own intrinsics, so
 * `frames[0].Element === Element` is false in Chrome and would be true here. The fix is per-component and lands
 * in each component's own init; there is no agent-level place to put it, which is exactly why it is not here. */
static void wpt_agent_init(JSContext *ctx, const char *doc_name, const char *origin,
                           const char *top_level_url)
{
    /* THE COMPONENTS UNDER TEST. Named one by one rather than "everything", because a component that is not
       installed makes its tests fail LOUDLY on a missing global — which is the honest report — while quietly
       installing a stand-in would make the gate agree with itself. Grows as areas are covered. */
    fetch_init(ctx);   /* §5/§6/§5.3, and §4 under them — one declaration point for the whole of Fetch */
    url_init(ctx);
    usp_init(ctx);
    form_data_init(ctx);
    readable_stream_init(ctx);
    queuing_strategy_init(ctx);
    /* §5.4's controller carries an AbortSignal, which is an EventTarget that DISPATCHES — so the three
       pieces that stack has to have all exist: the listener key, the Event class the `abort` event is an
       instance of, and the signal's own slot. Installing the signal without the Event class left the abort
       path minting an event out of a class that had never been built. */
    event_target_init(ctx);
    event_init(ctx);
    report_exception_init(ctx);
    /* NAME THIS DOCUMENT. A WindowProxy answers "is this navigable remote?" by comparing against the one
       document identity the world registry owns, so the registry has to be up before the first proxy exists. */
    world_registry_init(doc_name);
    /* THE CONCOLIC VALUE CLASS. This runner exercises the REAL components, and several of them answer with a
       concolic value where the spec's answer is genuinely unknown input — `window.name` survives navigation, so
       an attacker who can open the document sets it. The class has to be registered before any component can
       mint one; without it every file here aborted at the first such read. */
    concolic_init(ctx);
    /* THE VALUE SEMANTICS, WITHOUT THE ABSENT-GLOBAL SOURCE. A conformance run reaches concolic values — the
       two Location sources are the document's own address — and every operator over one needs the hooks or it
       throws at the first coercion. What it must NOT have is a global that was never set becoming unknown
       input: the spec answers that with a ReferenceError, and the corpus tests exactly that. */
    concolic_install_hooks();
    /* THE SOLVER FRONTIER, because a member that SUSPENDS parks on a flow's pending register and that register
       is the frontier's. This runner drives quickjs flows (JS_FlowNew/JS_FlowResume) but had no solver flow at
       all, so §7.4's open() aborted on "issued outside a flow" — correctly. One flow, set running for the whole
       run: this harness exercises components, and the BFS that would rank many of them is the engine's. */
    flow_registry_init(doc_name);
    flow_set_running(flow_add(ctx, JS_UNDEFINED, NULL, 0, WORLD_NONE));
    window_init(ctx);
    /* §7.2.4's Location, DECLARED here and built per realm by the intrinsic it registers. This runner used to
       BUILD ITS OWN out of the address — eight members, its own put/prefix helpers, its own parse — because
       location.c declares `search` and `hash` as concolic attacker SOURCES and a conformance run needs the real
       strings; that was a second component answering `location`, and it cost exactly what a second component
       costs (the §7.2.4 stringifier landed in location.c and this realm never saw it, so `new URL(path,
       location)` went on handing the URL parser `[object Object]` and four files ended at their first import).
       The source carries the example the ADDRESS actually has, so there is one Location and the overlay this
       host declines no longer removes the value. AFTER concolic_init, which registers the class a source is. */
    location_init(ctx);
    /* HTML §7.4.1's SESSION HISTORY and §7.2.5's History, DECLARED here and built per realm by the intrinsics
       they register. The state machine goes FIRST: realm.h runs the intrinsics in declaration order, and every
       member of History reads the record session_history's install builds. This is what a client-side router
       navigates with — `history.pushState()` is how React Router, Vue Router, Angular and every hand-rolled
       router change route — so without it a routing bundle threw on its first navigation and every route, lazy
       chunk and endpoint behind one went unexplored. */
    session_history_init(ctx);
    history_init(ctx);
    navigable_init(ctx);
    /* HTML §8.1.7's EVENT LOOP, before the task sources that are ordered by it: the virtual clock, §8.1.7.1's
       last render opportunity time and the insertion order a source breaks its ties by are the LOOP's, and
       they are per-flow heap state, so the record has to exist before any flow can write one. */
    event_loop_init(ctx);
    timer_init(ctx);
    window_proxy_init(ctx, origin);
    /* §7.2.5.1 one agent further out: a same-origin cross-document read answers with an OBJECT, and an object
       crosses as a NAME. Both halves live here — this agent lending its own, and referencing a peer's. */
    remote_object_init(ctx);
    /* AFTER the proxy class: §9.4.4's `postMessage` is declared once and installed on the WindowProxy
       PROTOTYPE, which window_proxy_init is what builds. */
    window_message_init(ctx);
    /* THE DOM CHOKEPOINT'S CONTEXT. §4.2.3's insertion and removing steps are fired from the solver's tree
       chokepoint, which needs the runtime they run in — and this runner never named one, so it ran NONE of
       them: no <script> preparation, no custom-element upgrade, no §4.8.5 child navigable. It failed
       silently, three layers away, as an iframe whose contentWindow was null. */
    dom_cow_set_ctx(ctx);
    element_init(ctx);
    iframe_init(ctx);   /* §4.8.5: the slot a child navigable lives in */
    document_init(ctx);
    message_port_init(ctx);
    xhr_init(ctx);   /* XHR §3, and §5's ProgressEvent under it */
    broadcast_channel_init(ctx, origin);
    abort_init(ctx);        /* the AbortSignal slot key §5.4's signal lives in */
    observable_init(ctx);
    writable_stream_init(ctx);
    transform_stream_init(ctx);
    blob_init(ctx);
    /* THE ONE VIRTUAL FILESYSTEM, and the File System Standard over it. The MODEL goes first (its two roots are
       built at this pre-boot baseline, so no flow's creation becomes every sibling's); §2.5's stream is
       DECLARED after §5's WritableStream because its prototype chains to that one and core/realm.h runs the
       per-realm installs in declaration order; §2.2-§2.4's handles after the stream because `createWritable()`
       mints one. §3's StorageManager is not here: this host builds no Navigator for `navigator.storage` to be a partial
       interface member of, so the interfaces exist and the bucket file system has no door in this entry. */
    file_system_init(ctx);
    fs_writable_init(ctx);
    fs_handle_init(ctx);
    encoding_init(ctx);
    /* §7.5 and §7.6 are the same codecs driven by a TransformStream, so they install AFTER §6 — the
       constructors reach it through transform_stream_op the moment a page builds one. */
    text_stream_init(ctx);
    /* HTML §8.1.7.3's IN-PARALLEL HALF — the rendering task source and "update the rendering", plus §8.9's
       map of animation frame callbacks and §7.4.6.3's reveal. */
    animation_frame_init(ctx);
    page_reveal_init(ctx);
    /* CSSOM VIEW §4, §12 and §13.1 — the viewport's Window extensions (`innerWidth`, `outerHeight`,
       `scrollY`, `screenLeft`, `devicePixelRatio`), the VisualViewport, and the per-realm record each keeps
       of what the RESIZE STEPS last saw. DECLARED before the rendering loop because update-the-rendering
       STEP 8 is their algorithm, and after §2.7 because VisualViewport.prototype chains to EventTarget's. */
    viewport_init(ctx);
    visual_viewport_init(ctx);
    /* CSSOM VIEW §4.2 and §7 — `matchMedia`, MediaQueryList and MediaQueryListEvent. DECLARED before the
       rendering loop because update-the-rendering STEP 10 is its algorithm, and after §2.7 and §2.2 because
       both of its prototypes chain to theirs. */
    media_query_list_init(ctx);
    rendering_init(ctx);
    /* THE AGENT'S FIRST REALM IS A REALM. Every per-realm intrinsic the components above declared is built
       here, through the same one call a child navigable's realm makes — so the first document cannot get a
       different set from the rest, which is the whole failure mode of a hand-copied list.
       IT CARRIES THE ENVIRONMENT (core/realm.h): §8.1.3.5 decides from the top-level creation URL whether
       this realm is a secure context, and Web IDL §3.3.13's members exist or do not by that answer — so it
       has to be known before the first intrinsic is installed, which is why it is an argument here. */
    realm_install_intrinsics(ctx, top_level_url);
}

/* ONE DOCUMENT. Runs once per document INCLUDING the first, which is what makes it the one description of what
   a document of this build is — a same-origin child navigable gets exactly this and nothing else. */
static void wpt_realm_install(JSContext *ctx, lxb_html_document_t *dom, const char *url, const char *origin,
                              const char *csp, uint32_t doc_id, JSValueConst nav_proxy)
{
    JSValue global = JS_GetGlobalObject(ctx);

    /* `self` IS NOT SET HERE. It is §7.2.2's [Replaceable] Window member and window_install below installs it
       as one; a plain own value written first was a STAND-IN for the member under test, and a stand-in is how
       a gate comes to agree with itself — this one would have hidden the member being absent entirely. */
    JS_SetPropertyStr(ctx, global, "print", JS_NewCFunction(ctx, js_wpt_print, "print", 1));
    /* `gc` — what /common/gc.js reaches for first, and the only way a test that asserts a stream survives
       COLLECTION can assert anything at all. It is the runner's, never the browser's: a page-visible collector
       is fingerprintable and this engine does not ship one. quickjs's own JS_RunGC is the whole of it. */
    JS_SetPropertyStr(ctx, global, "gc", JS_NewCFunction(ctx, js_wpt_gc, "gc", 0));

    headers_install(ctx, global);
    response_install(ctx, global);
    request_install(ctx, global);
    url_install(ctx, global);
    usp_install(ctx, global);
    form_data_install(ctx, global);
    readable_stream_install(ctx, global);
    queuing_strategy_install(ctx, global);
    event_install(ctx, global);
    /* §2.7: the global reaches add/removeEventListener/dispatchEvent through Window.prototype ->
       EventTarget.prototype, which window_install chains it to. */
    /* WEB IDL §3.6's [Global] rule needs to know WHICH object is the window, and this runner never said. Every
       unqualified `addEventListener(...)` in the corpus — which is how most of it registers — resolved to an
       undefined receiver and registered on nothing. */
    structured_clone_install(ctx, global);   /* HTML 2.7.3, and what 9.4 and §4.9.7 clone through */
    message_event_install(ctx, global);
    error_event_install(ctx, global);
    /* HTML §7.2.2's BROWSING-CONTEXT MEMBERS — window, self, frames, parent, top, opener, closed, origin and
       name. This runner had none of them, so `window` itself was undefined and every test in
       html/browsers/the-window-object failed on its first line. */
    window_install(ctx, global, origin);
    navigable_install(ctx, global, origin);   /* HTML 7.4 */
    /* THE DOCUMENT COMES AFTER THE BROWSING CONTEXT, not before it. §4.8.5's insertion steps run during tree
       construction, so installing the document CREATES a child navigable for every <iframe> the markup
       contains — which needs the WindowProxy class and §7.4's create-a-navigable to exist first. The engine's
       own host has always installed them in this order; this runner had it backwards and the assert in
       window_proxy_new_remote said so the moment a parsed iframe reached it.
       It could not simply be added either: a document makes testharness take its WINDOW path, which arms a
       wall-clock timeout for the whole file, and under a virtual clock that timeout used to arrive the instant
       the queue drained — landing in the middle of the tests it guards and reporting 140 passing stream
       subtests as timeouts. It cannot now: a timer is due only when the event loop has nothing else to run
       (timer.h), so the long timeout is by construction the last thing to happen. */
    window_message_install(ctx, global, origin);
    document_install(ctx, global, dom, url, csp, doc_id, nav_proxy);
    animation_frame_install(ctx, global);   /* HTML §8.9: requestAnimationFrame/cancelAnimationFrame */
    page_reveal_install(ctx, global);       /* HTML §7.4.6.3: PageRevealEvent */
    media_query_list_install(ctx, global);   /* CSSOM VIEW §4.2/§7: matchMedia, MediaQueryList */
    message_port_install(ctx, global);   /* HTML 9.4.2/9.4.3 */
    xhr_install(ctx, global);            /* XHR §3, §5 */
    broadcast_channel_install(ctx, global);   /* HTML 9.5 */
    abort_install(ctx, global);
    observable_install(ctx, global);
    writable_stream_install(ctx, global);
    transform_stream_install(ctx, global);
    blob_install(ctx, global);
    encoding_install(ctx, global);
    text_stream_install(ctx, global);
    /* HTML 8.6's TIMER TASK SOURCE. The runner had none, so `setTimeout` was simply absent — and testharness
       arms its own timeout with one, which is how a file whose tests were still pending ended a run with
       nothing reported and every unsettled promise's reactions leaked. A timer enqueues a JOB, so the pump
       below already drives them; what was missing was only the globals. */
    timer_install(ctx, global);
    fetch_install(ctx, global);
    { static const FetchProvider P = { wpt_owe }; fetch_set_provider(&P); }

    /* NO location_install: §7.2.4's Location is a per-realm intrinsic, so `location`, `Location` and
       `Location.prototype` are already on this global — realm_install_intrinsics put them there, and it reads
       the address off the DOCUMENT at each member call, which is why it can be built before document_install
       above has decided what this realm's document is. */
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
/* The foreign world's segment currently INSTALLED in this instance — the child's answer to "which flow is
   running", because its serve loop is its scheduler. */
static CowDelta *g_cur_seg;

/* WHICH OPERATION A RECORD IS. One enumeration for the whole channel, because the FIELD LAYOUT and the field
   COUNT are both facts about the operation and stating them apart is how the id came back as zero once already. */
enum { WPT_OP_POST, WPT_OP_WPGET, WPT_OP_GET, WPT_OP_SET, WPT_OP_DELETE, WPT_OP_APPLY };

static int wpt_op_of(const char *verb)
{
    if (!strcmp(verb, "windowproxy.post")) return WPT_OP_POST;
    if (!strcmp(verb, "windowproxy.get"))  return WPT_OP_WPGET;
    if (!strcmp(verb, "object.get"))       return WPT_OP_GET;
    if (!strcmp(verb, "object.set"))       return WPT_OP_SET;
    if (!strcmp(verb, "object.delete"))    return WPT_OP_DELETE;
    if (!strcmp(verb, "object.apply"))     return WPT_OP_APPLY;
    DFAIL("a cross-instance record named an operation this instance does not perform — an unanswered record "
          "parks the asking flow forever, so the operation has to be built rather than ignored");
    return WPT_OP_WPGET;
}

/* THE FIELDS OF ONE RECORD, SPLIT ONCE AND WITHOUT A CEILING. The split was a fixed array of eight, which is a
   cap on how many ARGUMENTS may cross — `object.apply` carries one field per argument — and the ninth would
   have been read as part of the eighth. One split, then index; `line` is modified in place and the caller frees
   the vector. */
static char **wpt_split(char *line, int *pn)
{
    char **f = NULL, *q = line;
    int n = 0, cap = 0;

    for (;;) {
        char *t = strchr(q, '\t');
        if (n == cap) {
            cap = cap ? cap * 2 : 8;
            f = realloc(f, (size_t)cap * sizeof *f);
            CHECK(f != NULL, "wpt: OOM splitting a cross-instance record");
        }
        f[n++] = q;
        if (!t) break;
        *t = 0;
        q = t + 1;
    }
    *pn = n;
    return f;
}

/* %Reflect.set% and %Reflect.apply% of THIS instance's realm, captured before its scripts run — see the
   capture in wpt_child_main for why the operation is not spelled in the peer-side program's text. */
static JSValue g_op_set = JS_UNDEFINED, g_op_apply = JS_UNDEFINED;

/* A CHILD CANNOT INITIATE, so its notices ride back on the answer pipe prefixed `N\t` and the parent's router
   takes them from there. Drained before every answer, because the answer is the only moment this instance is
   allowed to speak — a notice held past it waits for the next question that may never come. */
static void wpt_child_emit_notices(void)
{
    const char *notices = engine_host_notices();
    const char *p;

    for (p = notices; *p; ) {
        const char *end = strchr(p, '\n');
        if (!end) break;
        fputs("N\t", stdout);
        fwrite(p, 1, (size_t)(end - p), stdout);
        fputc('\n', stdout);
        p = end + 1;
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

    JS_SetFlowControlHooks(&WPT_HOOKS_ON);

    /* THE OPERATIONS THIS INSTANCE PERFORMS ON BEHALF OF A PEER, captured BEFORE the document's own scripts
       run and handed to each program through a SLOT.
       WHY NOT `Reflect.set(…)` SPELLED IN THE PROGRAM'S TEXT. `Reflect` is a global and the page may replace
       it, so a peer answering a cross-agent [[Set]] through the page's own function would report a write that
       never happened and would run the page's code where the spec puts an internal method. Two of the four
       operations have a pure-syntax form nothing can intercept — `o[k]` IS 10.1.8 and `delete o[k]` IS 10.1.10,
       and a SLOPPY-mode `delete` yields exactly the boolean the trap owes — and the two that do not reach the
       operation through the intrinsic captured here: an assignment expression discards the boolean 10.1.9
       completes with, and a call needs its argument list spread. */
    {
        static const char SET_SRC[] = "Reflect.set", APPLY_SRC[] = "Reflect.apply";
        CHECK(!run_program_value(ctx, SET_SRC, sizeof SET_SRC - 1, "<cross-agent intrinsic>", &g_op_set) &&
              !run_program_value(ctx, APPLY_SRC, sizeof APPLY_SRC - 1, "<cross-agent intrinsic>", &g_op_apply),
              "wpt: a child document's realm has no Reflect.set / Reflect.apply — a peer performs a cross-agent "
              "write and call through them, and a realm without them can answer neither");
    }

    {
        DocScripts ds = document_exec_scripts(g_wpt_dom);
        int i;
        for (i = 0; i < ds.n; i++) {
            if (ds.bodies[i]) run_program(ctx, ds.bodies[i], strlen(ds.bodies[i]), g_base_url);
            else if (ds.srcs[i]) {
                size_t len = 0;
                char *body = wpt_get(ds.srcs[i], &len);
                if (!body) continue;
                run_program(ctx, body, len, ds.srcs[i]);
                free(body);
            }
        }
        doc_scripts_free(&ds);
    }

    while (fgets(line, sizeof line, stdin)) {
        char *nl = strchr(line, '\n');
        /* A RECORD THAT DID NOT FIT arrived as two, and the tail would then be read as an operation of its own.
           The channel is line-oriented, so the only safe answer to a truncated read is to stop. */
        CHECK(nl != NULL || feof(stdin), "wpt: a cross-instance record was longer than this child's line buffer");
        JSValue v = JS_UNDEFINED;
        WorldId w = WORLD_NONE, anc[16];
        const char *worlds, *prog = NULL;
        char *enc, **f;
        int n_anc = 0, nf = 0, op, need, completion = ENGINE_COMPLETION_NORMAL;

        if (nl) *nl = 0;
        f = wpt_split(line, &nf);
        op = wpt_op_of(f[0]);
        /* HOW MANY FIELDS EACH OPERATION IS, declared beside the operation rather than at the read: a record
           shorter than its operation used to be SKIPPED, which parks the asking flow on an answer that never
           comes and reports nothing at all. */
        need = op == WPT_OP_POST ? 5 : op == WPT_OP_WPGET ? 4 : op == WPT_OP_SET ? 6 : 5;
        DCHECK(nf >= need, "a cross-instance record arrived with fewer fields than its operation carries — the "
                           "writer and this reader disagree about the grammar, and answering from the fields "
                           "that did arrive would answer a different question");
        if (nf < need) { free(f); continue; }   /* in release there is no field to read; reading one is worse */
        /* `windowproxy.post <world> <sender origin> <target origin> <base64 bytes>` — §9.4.4 step 7 arriving
           from another instance. It is not a read: nothing is asked of this document, a delivery is MADE to it,
           so its vector is the FIRST field where every other operation names a target document first. */
        worlds = (op == WPT_OP_POST) ? f[1] : f[2];

        /* THE WORLD THE ANSWER IS TRUE IN, and the ancestry that lets this instance HAVE one. A world minted
           in another document has a segment here only if a flow of that world has written here; the ancestry
           is what makes the segment inherit what the flow's ANCESTORS wrote, by forking the nearest one this
           instance already holds. Without it every cross-document operation would answer from a baseline the
           asking flow left behind — which is not a stale answer, it is a different timeline. It is what makes a
           WRITE tractable at all: two arms of a fork writing through one reference are two timelines, and one
           baseline for both would be a third neither arm was in. */
        n_anc = world_parse(worlds, &w, anc, (int)(sizeof anc / sizeof anc[0]));

        /* THE CONTEXT SWITCH, done here because this loop IS this instance's scheduler: unapply whatever world
           the last answer ran under, install this one's segment, answer, and leave it installed for the next
           question — which is the same "move only as far as the two differ" the engine's own switch does. */
        if (!world_is_none(w)) {
            CowDelta *seg = world_segment(ctx, w, anc, n_anc);
            if (seg != g_cur_seg) {
                cow_unapply(ctx, g_cur_seg);
                g_cur_seg = seg;
                cow_set_current(seg);
                cow_apply(ctx, seg);
            }
        }

        if (op == WPT_OP_POST) {
            /* THE DELIVERY, under the world just installed, and taken apart by the file that WROTE the record.
               The decode and the sender-document split were done here by hand, which is window_message.c's
               grammar restated where nothing can check it against the writer; the head of the world vector
               names the sender, so world_parse above already answered that too. */
            size_t cap = strlen(f[3]) + strlen(f[4]) + 2;
            char *tail = malloc(cap);

            CHECK(tail != NULL, "wpt: OOM receiving a routed message");
            snprintf(tail, cap, "%s\t%s", f[3], f[4]);
            window_message_route(ctx, tail, world_doc_name(w.doc), f[2]);
            free(tail);
            /* THE TASK IS ENQUEUED, NOT RUN: the scheduler is what runs it, and entering it is what a turn of
               this loop does. An empty program is that turn — it drains the posted-message task source the way
               any other turn would, rather than this host driving the delivery itself. */
            run_program(ctx, "", 0, "<routed message>");
            wpt_child_emit_notices();
            /* THE ACK IS A COMPLETION TOO. Nothing is asked of a delivery, so it is the normal completion of
               `undefined` — written through the one encoder so this pipe has ONE grammar rather than one for
               the answers and a bare `u` for this. */
            {
                char *ack = remote_completion_encode(ctx, ENGINE_COMPLETION_NORMAL, JS_UNDEFINED);
                fputs(ack, stdout);
                fputc('\n', stdout);
                free(ack);
            }
            fflush(stdout);
            free(f);
            continue;
        }

        /* WHAT IS BEING PERFORMED. `windowproxy.get` reads a member of THIS document's Window; the four
           `object.*` operations are the internal methods of an object this document LENT OUT. Every one of them
           is performed by running a PROGRAM, because an IDL accessor, a page's setter and a page's function are
           the page's code and a C activation has no flow base under it.
           EVERY OPERAND REACHES THE PROGRAM THROUGH A SLOT, the property name included. It used to be spliced
           into the text inside double quotes, which is a property name READ AS CODE the first time one carries
           a quote or a backslash — and the comment beside it claimed the opposite. */
        {
            JSValue g = JS_GetGlobalObject(ctx);

            if (op == WPT_OP_WPGET) {
                JS_SetPropertyStr(ctx, g, "__apiclientKey", JS_NewString(ctx, f[3]));
                prog = "globalThis[__apiclientKey]";
            } else {
                JSValueConst held = remote_object_by_id((uint32_t)strtoul(f[3], NULL, 10));
                DCHECK(!JS_IsUndefined(held),
                       "a peer named an object this agent never lent — the name it used was minted somewhere "
                       "else, or the export table was lost between the lend and the operation");
                JS_SetPropertyStr(ctx, g, "__apiclientLent", JS_DupValue(ctx, held));
                if (op == WPT_OP_APPLY) {
                    JSValue args = JS_NewArray(ctx);
                    int i;
                    /* THE ARGUMENT LIST IS FLAT ON THE WIRE — one field per argument, in the one grammar — so
                       there is no second grammar for a list and no ceiling on how many may cross. */
                    for (i = 5; i < nf; i++)
                        JS_SetPropertyUint32(ctx, args, (uint32_t)(i - 5), remote_object_decode(ctx, f[i]));
                    JS_SetPropertyStr(ctx, g, "__apiclientThis", remote_object_decode(ctx, f[4]));
                    JS_SetPropertyStr(ctx, g, "__apiclientArgs", args);
                    JS_SetPropertyStr(ctx, g, "__apiclientOp", JS_DupValue(ctx, g_op_apply));
                    prog = "__apiclientOp(__apiclientLent, __apiclientThis, __apiclientArgs)";
                } else {
                    JS_SetPropertyStr(ctx, g, "__apiclientKey", remote_object_decode(ctx, f[4]));
                    if (op == WPT_OP_SET) {
                        JS_SetPropertyStr(ctx, g, "__apiclientVal", remote_object_decode(ctx, f[5]));
                        JS_SetPropertyStr(ctx, g, "__apiclientOp", JS_DupValue(ctx, g_op_set));
                        prog = "__apiclientOp(__apiclientLent, __apiclientKey, __apiclientVal)";
                    } else if (op == WPT_OP_DELETE) {
                        /* SLOPPY MODE ON PURPOSE: `delete` yields 10.1.10's boolean here and THROWS for a false
                           in strict mode, and the boolean is exactly what 10.5.10 step 8 asks the trap for. */
                        prog = "delete __apiclientLent[__apiclientKey]";
                    } else {
                        prog = "__apiclientLent[__apiclientKey]";
                    }
                }
            }
            JS_FreeValue(ctx, g);
        }
        {
            int failed;
            DCHECK(prog != NULL, "a cross-instance operation reached the peer's program runner with no program "
                                 "— an operation this loop routes but does not perform");
            failed = run_program_value(ctx, prog, strlen(prog), "<cross-agent operation>", &v);
            /* THE THROW IS THE ANSWER. The peer performs every operation by running a program — an IDL
               accessor, the page's setter, the page's function — and a program that throws has completed just
               as truly as one that returned. It is NOT reported as this instance's error: it is encoded with
               its completion type and raised in the flow that asked, at the line that asked, so that page's
               own `try`/`catch` runs. The thrown value crosses by the ordinary rules, so an Error object
               crosses as a NAME and the catch clause holds a reference to THIS instance's Error. */
            completion = failed ? ENGINE_COMPLETION_THROW : ENGINE_COMPLETION_NORMAL;
        }
        enc = remote_completion_encode(ctx, completion, v);
        JS_FreeValue(ctx, v);
        wpt_child_emit_notices();
        fputs(enc, stdout);
        fputc('\n', stdout);
        fflush(stdout);
        free(enc);
        free(f);
    }
    cow_unapply(ctx, g_cur_seg);
    cow_set_current(NULL);
    JS_FreeValue(ctx, g_op_set);
    JS_FreeValue(ctx, g_op_apply);
    g_op_set = g_op_apply = JS_UNDEFINED;
    JS_SetFlowControlHooks(&WPT_HOOKS_OFF);
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

    rendering_free(ctx);
    page_reveal_free(ctx);
    media_query_list_free(ctx);
    viewport_free();
    visual_viewport_free();
    location_free();
    session_history_free();
    history_free();
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
    return failed ? 1 : 0;
}
