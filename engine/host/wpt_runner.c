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
#include "core/streams/queuing_strategy.h"
#include "core/events/event.h"
#include "core/events/message_event.h"
#include "core/events/message_port.h"
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
#include "solver/concolic.h"
#include "solver/flow.h"
#include "solver/engine.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include "core/timing/timer.h"
#include <lexbor/html/html.h>
#include "core/dom/element.h"
#include "core/dom/document.h"
#include "core/loader/document_scripts.h"
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
static void wpt_spawn_child(const char *name, const char *url, const char *origin, const char *csp)
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
        execl(g_self_exe, g_self_exe, "--document", name, url, origin, csp ? csp : "", (char *)NULL);
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

static bool wpt_child_ask(ChildDoc *c, const char *op, char *out, size_t cap)
{
    static char *buf;
    static size_t buf_cap;

    wpt_send_all(c->to, op, strlen(op));
    wpt_send_all(c->to, "\n", 1);
    for (;;) {
        if (!wpt_child_read_line(c, &buf, &buf_cap)) return false;
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
        snprintf(out, cap, "%s", buf);
        return true;
    }
}

/* WHICH AGENT the answer being decoded came from — an object name is only meaningful in the name space of the
   agent that minted it, so the decode has to know whose it is. Set around the one ask. */
static uint32_t g_answering_doc;

/* The typed answer text -> the value the asking flow resumes with. */
static JSValue wpt_decode_answer(JSContext *ctx, const char *a)
{
    switch (a[0]) {
    case 'b': return JS_NewBool(ctx, a[1] == '1');
    case 'n': return JS_NewFloat64(ctx, strtod(a + 1, NULL));
    case 's': return JS_NewString(ctx, a + 1);
    case 'N': return JS_NULL;
    case 'u': return JS_UNDEFINED;
    case 'o': {
        /* THE PEER NAMED ONE OF ITS OBJECTS. `doc` is which agent's name space the id is in — this decode is
           only ever reached for an answer that came back from `c`, so it is that child's. */
        DCHECK(g_answering_doc != 0, "a cross-agent name was decoded with no agent to attribute it to");
        return remote_object_ref(ctx, g_answering_doc, (uint32_t)strtoul(a + 1, NULL, 10));
    }
    default:
        DFAIL("a child document answered with a value this protocol does not name");
        return JS_UNDEFINED;
    }
}

static void wpt_encode_answer(JSContext *ctx, JSValueConst v, char *out, size_t cap)
{
    if (JS_IsBool(v))        snprintf(out, cap, "b%d", JS_ToBool(ctx, v) ? 1 : 0);
    else if (JS_IsNull(v))   snprintf(out, cap, "N");
    else if (JS_IsUndefined(v)) snprintf(out, cap, "u");
    else if (JS_IsNumber(v)) { double d = 0; JS_ToFloat64(ctx, &d, v); snprintf(out, cap, "n%.17g", d); }
    else if (JS_IsString(v)) {
        const char *t = JS_ToCString(ctx, v);
        snprintf(out, cap, "s%s", t ? t : "");
        if (t) JS_FreeCString(ctx, t);
    } else if (JS_IsObject(v)) {
        /* AN OBJECT CROSSES AS ITS NAME. This agent lends it — one id per object, so the asker's reference has
           the identity a page compares — and the asker resolves the name to the one reference it keeps for it. */
        snprintf(out, cap, "o%u", remote_object_export(ctx, v));
    } else {
        DFAIL("a child document was asked for a member whose value is neither a primitive nor an object");
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
        char ack[16];
        CHECK(op != NULL, "wpt: OOM routing a message to a child document");
        snprintf(op, cap, "windowproxy.post\t%s\t%s\t%s\t%s", world, from_origin, target_origin, b64);
        wpt_child_ask(c, op, ack, sizeof ack);
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
    char *f[6]; int nf = 0; char *q;

    for (q = line, f[nf++] = q; *q && nf < 6; q++)
        if (*q == '\t') { *q = 0; f[nf++] = q + 1; }
    if (nf == 6 && !strcmp(f[0], "navigable.create")) {
        wpt_spawn_child(f[1], f[3], f[4], f[5]);
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
        char op[512], answer[512];
        uint32_t id;
        size_t n;

        if (!tab || !end) break;
        id = (uint32_t)strtoul(p, NULL, 10);
        n = (size_t)(end - tab - 1);
        /* BOTH CROSS-AGENT READS ROUTE THE SAME WAY — a member of a navigable's Window, and a property of an
           object that Window lent out. They differ in what the peer resolves, not in who resolves it. */
        if (n < sizeof op && (!strncmp(tab + 1, "windowproxy.get\t", 16) ||
                              !strncmp(tab + 1, "object.get\t", 11))) {
            char *doc, *q;
            ChildDoc *c;
            memcpy(op, tab + 1, n); op[n] = 0;
            doc = strchr(op, '\t') + 1;
            q = strchr(doc, '\t');
            if (q) {
                *q = 0;
                c = wpt_child_for(doc);
                *q = '\t';
                /* A READ FOR A DOCUMENT NOTHING PROVISIONED is not a slow answer, it is a missing instance —
                   the notice that named it was dropped, or the read outran it. Left unanswered the flow parks
                   forever and the file times out with nothing said, so it says it. */
                DCHECK(c != NULL, "a cross-document read named a document this host never provisioned — the "
                                  "navigable.create notice for it was dropped");
                if (c && wpt_child_ask(c, op, answer, sizeof answer)) {
                    JSValue v;
                    /* An object NAME in the answer belongs to the agent that answered, and nothing else in the
                       line says which that is. */
                    g_answering_doc = world_doc_intern(c->name);
                    v = wpt_decode_answer(ctx, answer);
                    g_answering_doc = 0;
                    engine_host_answer(ctx, id, v);
                    answered = true;
                    JS_FreeValue(ctx, v);
                }
            }
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
            engine_host_answer(ctx, id, v);
            answered = true;
            JS_FreeValue(ctx, v);
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

    CHECK(n < sizeof g_wpt_root, "wpt: the corpus root is longer than this runner holds");
    memcpy(g_wpt_root, h, n);
    g_wpt_root[n] = 0;
    /* THE LAST argument is the test; the ones between are its META scripts. Reading argv[2] made a file with a
       META script resolve its data against that script's directory instead of its own. */
    if (argc > 2 && !strncmp(test, g_wpt_root, n))
        snprintf(g_base_url, sizeof g_base_url, "http://web-platform.test%s", test + n);
    else
        snprintf(g_base_url, sizeof g_base_url, "http://web-platform.test/");
    if (g_html_mode) {
        CHECK(!strncmp(test, g_wpt_root, n), "wpt: an HTML test outside the corpus root has no server address");
        snprintf(g_test_url, sizeof g_test_url, "http://web-platform.test%s", test + n);
        snprintf(g_base_url, sizeof g_base_url, "%s", g_test_url);
    }
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
   every back-edge. A throw is reported and ends the file — a test file that cannot run is a failure, never a
   skip. */
/* Run one program as a flow and KEEP ITS VALUE. A member read answered on behalf of another document is a
   program like any other and must run as a flow like any other: reading `self.length` from C would reach the
   getter with no flow base under it, which is the one thing this engine refuses. `*pres` is owned. */
static int run_program_value(JSContext *ctx, const char *src, size_t len, const char *name, JSValue *pres)
{
    JSValue *flow = JS_FlowNew(ctx, src, len, name, JS_EVAL_TYPE_GLOBAL);
    JSValue res = JS_UNDEFINED;
    int failed = 0;

    if (pres) *pres = JS_UNDEFINED;
    if (!flow) {
        wpt_report_exception(ctx, name, "compile: ");
        return 1;
    }
    while (JS_FlowResume(ctx, flow, &res)) { wpt_answer_host_requests(ctx); }
    if (JS_IsException(res)) {
        wpt_report_exception(ctx, name, "");
        failed = 1;
    }
    if (pres && !failed) *pres = res;
    else JS_FreeValue(ctx, res);
    JS_FlowFree(ctx, flow);
    return failed;
}

static int run_program(JSContext *ctx, const char *src, size_t len, const char *name)
{
    return run_program_value(ctx, src, len, name, NULL);
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
static void wpt_agent_init(JSContext *ctx, const char *doc_name, const char *origin)
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
    message_event_init(ctx);
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
    navigable_init(ctx);
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
    broadcast_channel_init(ctx, origin);
    abort_init(ctx);        /* the AbortSignal slot key §5.4's signal lives in */
    writable_stream_init(ctx);
    transform_stream_init(ctx);
    blob_init(ctx);
    encoding_init(ctx);
    /* §7.5 and §7.6 are the same codecs driven by a TransformStream, so they install AFTER §6 — the
       constructors reach it through transform_stream_op the moment a page builds one. */
    text_stream_init(ctx);
    /* THE AGENT'S FIRST REALM IS A REALM. Every per-realm intrinsic the components above declared is built
       here, through the same one call a child navigable's realm makes — so the first document cannot get a
       different set from the rest, which is the whole failure mode of a hand-copied list. */
    realm_install_intrinsics(ctx);
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
    message_port_install(ctx, global);   /* HTML 9.4.2/9.4.3 */
    broadcast_channel_install(ctx, global);   /* HTML 9.5 */
    abort_install(ctx, global);
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

    /* THE CORPUS ROOT AND THE DOCUMENT ADDRESS. The driver hands this runner
       `<root>/resources/testharness.js` first and the test file second, so the root is what precedes the one
       and the address is the other, made server-relative — which is exactly what WPT's server serves from and
       resolves against. A worker's `location` is a real WorkerLocation. */
    /* THE ONE Location COMPONENT. This runner used to BUILD ITS OWN out of the address — eight members, its
       own put/prefix helpers, its own parse — because location.c declares `search` and `hash` as concolic
       attacker SOURCES and a conformance run needs the real strings. That is a second component answering
       `location`, and it cost exactly what a second component costs: the §7.10.5 stringifier was added to
       location.c and this realm never saw it, so `new URL(path, location)` went on handing the URL parser
       `[object Object]` and four files ended at their first import. The source now carries the example the
       ADDRESS actually has, so there is one Location and the concolic overlay no longer removes the value. */
    location_install(ctx, global, url);
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
                                  const char *origin, const char *csp, uint32_t doc_id,
                                  JSValueConst nav_proxy)
{
    JSContext *ctx = JS_NewContext(rt);

    CHECK(ctx != NULL, "wpt: a same-origin child navigable's realm could not be created");
    /* §3.7: a realm gets its OWN intrinsics — the members on them run in the realm that DEFINED them, so a
       child sharing the agent realm's EventTarget.prototype would resolve every unqualified
       `addEventListener` against the PARENT's window. They come from the ONE list the components declared
       themselves into, so a component added anywhere is installed in every realm with no host to edit. */
    realm_install_intrinsics(ctx);
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
static JSContext *wpt_build_document(const char *doc_name, const char *origin,
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
    wpt_agent_init(ctx, doc_name, origin);
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
 * THE WORLD ARRIVES AND IS NOT YET USED, and that is stated rather than hidden: the request carries the asking
 * flow's world NAME, and world.h's segment materialization needs its ANCESTRY to fork the nearest ancestor a
 * peer already holds. Nothing sends the ancestry yet, so a peer holds none of it and starts from an empty
 * segment — which world.c says is the TRUTH for a world that has never written here, and becomes a lie the
 * moment a flow forks after writing in this document. That is the next mechanism, and it is why the world is
 * on the wire already. */
/* The foreign world's segment currently INSTALLED in this instance — the child's answer to "which flow is
   running", because its serve loop is its scheduler. */
static CowDelta *g_cur_seg;

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
    snprintf(g_base_url, sizeof g_base_url, "%s", url && *url ? url : "about:blank");
    /* `about:blank` HAS NO BYTES TO FETCH — its Document is the empty one §7.4 creates, which is what makes it
       synchronous in a browser and what makes an <iframe> with no src scriptable immediately. */
    if (strncmp(g_base_url, "about:", 6) != 0) {
        html = wpt_get(g_base_url, &html_n);
        /* A child whose address does not load still HAS a document — a browser shows an error page and the
           navigable exists — so this is about:blank rather than a dead instance. */
        if (!html) html_n = 0;
    }
    ctx = wpt_build_document(name, origin, html, html_n);
    free(html);

    JS_SetFlowControlHooks(&WPT_HOOKS_ON);
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
        char *nl = strchr(line, '\n'), *member, *worlds;
        /* A RECORD THAT DID NOT FIT arrived as two, and the tail would then be read as an operation of its own.
           The channel is line-oriented, so the only safe answer to a truncated read is to stop. */
        CHECK(nl != NULL || feof(stdin), "wpt: a cross-instance record was longer than this child's line buffer");
        JSValue v = JS_UNDEFINED;
        char prog[128], answer[512];
        WorldId w = WORLD_NONE, anc[16];
        int n_anc = 0, nf = 0, is_obj;
        char *f[8];

        if (nl) *nl = 0;
        /* THE FIELDS, SPLIT ONCE. `windowproxy.get <doc> <world> <member>` and
           `object.get <doc> <world> <id> <member>` differ by one field, and reading them with a pair of
           strrchr walks meant every field's end depended on which NUL a previous walk had written — the id
           came back as 0 because the world's terminator had eaten it. One split, then index. */
        {
            char *q;
            nf = 0;
            for (q = line, f[nf++] = q; *q && nf < 8; q++)
                if (*q == '\t') { *q = 0; f[nf++] = q + 1; }
        }
        is_obj = !strcmp(f[0], "object.get");
        /* `windowproxy.post <world> <sender origin> <target origin> <base64 bytes>` — §9.4.4 step 7 arriving
           from another instance. It is not a read: nothing is asked of this document, a delivery is MADE to it,
           so it answers a bare ack and the message becomes a task like any local post. */
        if (!strcmp(f[0], "windowproxy.post")) {
            if (nf < 5) continue;
            worlds = f[1];
            member = NULL;
        } else {
            if (nf < (is_obj ? 5 : 4)) continue;
            worlds = f[2];
            member = f[is_obj ? 4 : 3];
        }

        /* THE WORLD THE ANSWER IS TRUE IN, and the ancestry that lets this instance HAVE one. A world minted
           in another document has a segment here only if a flow of that world has written here; the ancestry
           is what makes the segment inherit what the flow's ANCESTORS wrote, by forking the nearest one this
           instance already holds. Without it every cross-document read would answer from a baseline the asking
           flow left behind — which is not a stale answer, it is a different timeline. */
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

        if (!member) {
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
            fputs("u\n", stdout);
            fflush(stdout);
            continue;
        }

        /* WHAT IS BEING READ. `windowproxy.get` reads a member of THIS document's Window; `object.get` reads a
           property of an object this document LENT OUT, and the field before the member names which. Both are
           read by running a PROGRAM, because both are IDL accessors and a C activation has no flow base under
           it — the object is handed to the program through a slot rather than spliced into its text, so a
           property name can never be read as code. */
        if (is_obj) {
            JSValueConst held = remote_object_by_id((uint32_t)strtoul(f[3], NULL, 10));
            DCHECK(!JS_IsUndefined(held), "a peer asked for a property of an object this agent never lent — the "
                                          "name it used was minted somewhere else or the export table was lost");
            {
                JSValue g = JS_GetGlobalObject(ctx);
                JS_SetPropertyStr(ctx, g, "__apiclientLent", JS_DupValue(ctx, held));
                JS_FreeValue(ctx, g);
            }
            snprintf(prog, sizeof prog, "__apiclientLent[%c%s%c]", '"', member, '"');
        } else {
            snprintf(prog, sizeof prog, "globalThis[%c%s%c]", '"', member, '"');
        }
        if (run_program_value(ctx, prog, strlen(prog), "<cross-document read>", &v)) v = JS_UNDEFINED;
        wpt_encode_answer(ctx, v, answer, sizeof answer);
        JS_FreeValue(ctx, v);
        wpt_child_emit_notices();
        fputs(answer, stdout);
        fputc('\n', stdout);
        fflush(stdout);
    }
    cow_unapply(ctx, g_cur_seg);
    cow_set_current(NULL);
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
    if (argc >= 2 && !strcmp(argv[1], "--test-document")) { g_html_mode = 1; argv++; argc--; }
    if (argc < 2) {
        fprintf(stderr, "usage: wpt_runner [--test-document] <testharness.js> <test> [more.js ...]\n");
        return 2;
    }
    /* WHICH FILE IS THE TEST, WHERE IT LIVES, AND WHETHER IT IS A DOCUMENT. Derived before anything else is
       built, because an HTML test's document has to be FETCHED and PARSED before `document` is installed —
       there is no second document to swap in later, and a runner that installed a placeholder first would be
       running the test's scripts against the wrong tree. */
    wpt_derive_addresses(argc, argv);
    /* THE TEST DOCUMENT. One call, because a child document is built by the same one — see above. */
    ctx = wpt_build_document("wpt", WPT_TOP_ORIGIN, NULL, 0);
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
        for (i = 1; i < argc && !failed; i++) {
            size_t len = 0;
            char *src = read_file(argv[i], &len);
            if (!src) { fprintf(report_out(), "@WPTERR %s: cannot read\n", argv[i]); failed = 1; break; }
            failed = run_program(ctx, src, len, argv[i]);
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
        /* NOTHING QUEUED AND NOTHING OWED, which is the one moment virtual time may move — see timer.h. A
           timer that is due fires here and its callback is a task the loop drains next. Only when this fires
           nothing either is the file really finished. */
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
    navigable_free(ctx);
    flow_registry_free(ctx);
    document_free(ctx);
    iframe_free(ctx);
    element_free(ctx);
    window_free(ctx);
    remote_object_free(ctx);
    window_proxy_free(ctx);
    world_registry_free(ctx);
    message_event_free(ctx);
    event_free(ctx);
    event_target_free(ctx);
    realm_intrinsics_free();   /* the DECLARATIONS are the agent's; each realm's prototypes went with it */
    queuing_strategy_free(ctx);
    readable_stream_free(ctx);
    blob_free(ctx);
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
