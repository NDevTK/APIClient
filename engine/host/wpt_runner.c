/* Web-platform-tests runner: the browser half's correctness gate.
 *
 * test262 is the JS half's oracle because every file carries its own asserts. WPT is the browser half's: the real
 * corpus and the real testharness.js at a pinned revision. A spec-conformance claim about the browser half means
 * "wpt passes"; the IDL audit measures coverage, not correctness, and a component's own fixture tests only what
 * its author thought of.
 *
 * It runs the feature engine and the product's execution model. Both documents this binary runs (the test
 * document and the child document it re-executes itself for) are on the one scheduler: engine_sched_begin seeds
 * the document's programs on the frontier, engine_sched_step is the only entry into JS, engine_route attaches a
 * delivery to every live timeline, and engine_perform makes a peer's operation a program of each. Preemption is
 * therefore the scheduler's policy (the value yield and the cooperative quantum), as in the extension.
 *
 * The programs load in order: resources/testharness.js, this runner's report (WPT_REPORT), the test, and for the
 * script arm an epilogue (WPT_EPILOGUE). Results come out as one @WPT line per subtest, which the driver reads. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "check.h"
#include "quickjs.h"
#include "quickjs-step.h"
#include "core/fetch/headers.h"
#include "core/fetch/response.h"
#include "core/fetch/request.h"
#include "core/fetch/fetch.h"
#include "core/mime/mime_type.h"   /* §4.2's essence: what this host states it served */
#include "core/url/origin.h"
#include "core/url/url.h"
#include "core/html/html_parse.h"   /* the one place a Document is parsed; that header owns the token bytes */
#include "core/loader/document_load_type.h"  /* §7.4.5: which document a response loads as, and its computed type */
#include "core/loader/document_load.h"       /* §7.4.5's load-a-document: the §7.5 subsection that arm runs */
#include "core/loader/document_load_decode.h" /* §13.2.3.2: which decoder that response's bytes need */
#include "core/frame/navigation_params.h"
#include "core/frame/policy_container.h"   /* §7.1.7's determine-navigation-params-policy-container */
#include "core/frame/secure_context.h"
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
#include "core/events/message_port.h"
#include "core/xhr/xml_http_request.h"
#include "core/events/broadcast_channel.h"
#include "core/frame/window.h"
#include "core/frame/window_proxy.h"
#include "core/frame/remote_location.h"
#include "core/frame/remote_object.h"
#include "core/frame/remote_op.h"
#include "core/html/html_iframe.h"
#include "core/frame/navigable_notice.h"   /* the provisioning notices' field tables */
#include "core/frame/navigable.h"
#include "solver/world.h"
#include "solver/cow.h"
#include "solver/novelty.h"   /* the endpoint surface's identity scope, stated by this host */
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
#include "solver/result.h"   /* who prints a page error while the programs are the scheduler's */
#include "solver/route_seed.h"   /* a document declaring one of its own pages — routed to a refusal here */
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <poll.h>
#include <errno.h>
#include "core/timing/timer.h"
#include "core/frame/viewport.h"
#include "core/frame/visual_viewport.h"
#include "core/rendering/rendering.h"
#include <lexbor/html/html.h>
#include "core/dom/element.h"
#include "core/geometry/dom_rect.h"
#include "core/geometry/dom_rect_list.h"
#include "core/dom/document.h"
#include "core/loader/document_scripts.h"
#include "core/frame/history.h"
#include "core/frame/session_history.h"
#include "core/frame/location.h"
#include "core/encoding/encoding.h"
#include "core/encoding/text_stream.h"
#include "core/idl_args.h"
#include "core/idl_async_iter.h"
#include "core/dom/node_interface.h"   /* the one place a Document is made; see that header */

/* Where a diagnostic goes. testharness.js reports through a callback and the callback through this, one line per
   subtest. In the parent, stdout carries the results the driver parses. In a child document stdout is the answer
   pipe, one line per question, so a page `print()` or an @WPTERR there would be read as an answer and every later
   answer would be one line behind, hanging teardown on a read that never comes. A child has no results, so its
   diagnostics go to stderr, which the driver merges into the file's output. */
static FILE *g_report;
static FILE *report_out(void) { return g_report ? g_report : stdout; }

/* Set when the harness ends the measurement (js_wpt_test_complete); read by the loop in main. */
static int g_test_complete;
/* Set once this run has asked the engine to write its frontier out. File scope because the notice router reads
   it: a park announces worlds, and such an announcement is expected only while the run is ending. See the loop in
   main. */
static int g_parking;

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

/* The harness ends the measurement: the vendor half of testharness's completion, called from WPT_REPORT.
 *
 * `done()` ends a WPT test: testharness sets its phase to COMPLETE, runs the completion callbacks and reports
 * nothing afterwards. WPT's model is that the runner then takes the browsing context away, which is HTML §7.3.1.6
 * "Navigable destruction"'s destroy a top-level traversable ("may destroy a top-level traversable at any time").
 * Destroy, not close: close fires `beforeunload` over the subtree and unloads it, which runs more of a program
 * that has already been measured.
 *
 * It is not a bound: every subtest has a result and the verdict is printed, so nothing measured is truncated.
 * What runs past it is a flow parked on a reply nobody will send, which is what the session's snapshot is for.
 * It is a call rather than a read of the printed @WPTDONE line because stdout is the driver's channel and a test
 * can forge a line with `print`. */
static JSValue js_wpt_test_complete(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)ctx; (void)this_val; (void)argc; (void)argv;
    DCHECK(g_report == NULL, "a CHILD document declared the measurement over — the file under test is the one "
                             "this process was started to run, and a child navigable's own harness ending "
                             "would take the parent's session down with a verdict nobody printed");
    /* Idempotent, because testharness does not promise one completion. `Tests.prototype.complete()` returns early
       only on `phase === COMPLETE`, which `all_complete` assigns at the end of an asynchronous path (`all_async`
       over the incomplete tests, each through `Test.prototype.cleanup`). A second `complete()` inside that window
       reaches its own `all_complete`, so `notify_complete` and every completion callback run twice. A second
       completion with a different verdict is a finding, and WPT_REPORT reports it. */
    g_test_complete = 1;
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

/* The prologue: the `.any.js` wrapper WPT's server generates, which a runner loading the file directly supplies
   itself. `self` is the global and `GLOBAL` names which global scope this is; a file's `// META: global=` line
   lists the variants it is written for. The script arm declares the worker variant because that is what it is: a
   global with fetch and Headers and no document under it. Declaring `window` would run document-dependent arms
   against no document. Every variant runs the whole file except the arms upstream marks window-only. */
static const char WPT_PROLOGUE[] =
    "self.GLOBAL = {\n"
    "  isWindow: function () { return false; },\n"
    "  isWorker: function () { return true; },\n"
    "  isShadowRealm: function () { return false; },\n"
    "};\n";


/* The report, installed as WPT's vendor hook at WPT's position.
 *
 * Every testharness document loads `/resources/testharnessreport.js` right after `/resources/testharness.js`, for
 * "vendors to implement code needed to integrate testharness.js tests with their own test systems". Registered
 * there, these callbacks are first in every list, which matters because notify_complete is a bare
 * `forEach(this.all_done_callbacks, callback)`: the first callback that throws skips every later one, and phase
 * is already COMPLETE. A corpus helper's completion callback can throw (IndexedDB/resources/support.js calls
 * `indexedDB.deleteDatabase`, which this engine does not have), and the throw is invisible: DOM §2.9 "Dispatching
 * events"' inner invoke reports a listener's exception instead of propagating it, and HTML §8.1.4.6 "Runtime
 * script errors"' developer console does not exist in this build.
 *
 * It streams: a subtest is announced when registered (@WPTSTART) and printed when its result is known (@WPT), so
 * a file that never completes still reports, and "none reported" differs from "none exist". The completion
 * callback prints the subtests the harness forced COMPLETE without a result (its timeout path) and the verdict.
 * `i` is the harness's `tests` index, so a re-announced or re-reported subtest is one fact. */
static const char WPT_REPORT[] =
    "(function () {\n"
    "  var announced = [], reported = [], verdict = null;\n"
    "  function emit(t) {\n"
    "    reported[t.index] = 1;\n"
    "    print('@WPT ' + JSON.stringify({ i: t.index, name: t.name, status: t.status, message: t.message }));\n"
    "  }\n"
    "  add_test_state_callback(function (t) {\n"
    /* The harness notifies test state at push and again at start. One line per subtest, because a corpus file
       can hold ninety thousand of them and this output is read through a fixed buffer. */
    "    if (announced[t.index]) return;\n"
    "    announced[t.index] = 1;\n"
    "    print('@WPTSTART ' + JSON.stringify({ i: t.index, name: t.name }));\n"
    "  });\n"
    "  add_result_callback(emit);\n"
    "  add_completion_callback(function (tests, status) {\n"
    "    for (var i = 0; i < tests.length; i++) if (!reported[tests[i].index]) emit(tests[i]);\n"
    "    var v = JSON.stringify({ status: status.status, message: status.message, count: tests.length });\n"
    "    print('@WPTDONE ' + v);\n"
    /* A second completion is the harness re-notifying (see js_wpt_test_complete) and is not a finding. One with a
       different verdict is, because the driver reads the presence of @WPTDONE and would hold two answers with
       nothing saying so. Every line is printed either way. */
    "    if (verdict !== null && verdict !== v)\n"
    "      print('@WPTERR the harness completed again with a DIFFERENT verdict than the one already printed: '\n"
    "            + verdict);\n"
    "    verdict = v;\n"
    /* The measurement ends after the verdict is printed. */
    "    wptTestComplete();\n"
    "  });\n"
    "})();\n";

/* The epilogue: the last line of WPT's worker wrapper, and so the script arm's alone.
 *
 * In `tools/serve/serve.py`, `ClassicWorkerHandler` (the `.any.worker.js` wrapper) is the GLOBAL shim, then
 * `importScripts("/resources/testharness.js")`, the test and `done();`, because a worker has no load event. The
 * script arm declares `GLOBAL.isWorker` (WPT_PROLOGUE), so it is that shape. `AnyHtmlHandler`, `WindowHandler` and
 * hand-written `.html` tests have no such line: the window's `load` event sets testharness's `all_loaded` from a
 * `setTimeout(…, 0)`, and `Tests.all_done()` completes the file.
 *
 * A document must not get it. Run as the last program, `done()` precedes the load event and every timer, and:
 *   - with no tests yet (they are created asynchronously) it stamps the harness error "done() was called without
 *     first defining any tests" and completes, so every later result is refused;
 *   - under `explicit_done` it completes at load instead of at the page's own `done()`, so later subtests NOTRUN;
 *   - under `single_test` it calls `tests.tests[0].done()` before the test's asynchronous work, a made-up verdict.
 * A document that never completes is therefore a fact about the engine. */
static const char WPT_EPILOGUE[] =
    "done();\n";

/* ---- The corpus as this host's network ------------------------------------------------------------------
 *
 * A wpt test that needs data fetches it (url-constructor reads its ~700 cases out of resources/urltestdata.json),
 * and WPT's own server answers off the checked-out tree. This host does the same through the real fetch
 * component: the request goes through §5.5, §5.3's Request and §5.1's guards as a page's would, and only who
 * satisfies it differs.
 *
 * Replies are delivered from the pump, not from `owe`: calling the deliver closure inside `owe` would be a JS_Call
 * from C with no flow base under it, which this engine aborts on. The owed reply is recorded and satisfied where
 * the runner already re-enters JS, as the trusted zone does. */
/* The corpus root, derived from the harness path the driver passes — the runner is handed
   `<root>/resources/testharness.js` as its first argument, so the root is what precedes it. */
static char g_wpt_root[512];


/* This host's copy of each owed request, because the reply seam does not carry the whole request.
 *
 * A `fetch()` parks its flow (engine_pending_fetch_url), which reports itself host-owed and resumes with the reply.
 * The register records method, headers and body, but engine_pending_fetches' line names only the method and the
 * URL, and engine_provide delivers against that pair. The corpus asks for POSTs answered by their body
 * (`echo-content.py`) and probes answered by their headers (`inspect-headers.py`), so this table keeps both,
 * keyed by the pair.
 * Named residual. Not covered: two outstanding requests with one method and URL that differ in body, which the
 * seam cannot tell apart. Next diff builds: an identity for the whole request on the line, and deletes this table.
 * Absence shows as: wpt_owe's body DCHECK firing. The strings are this runner's copies, because the component's
 * request is gone by the time the scheduler stalls on it. */
#define WPT_OWED_MAX 64
static struct {
    char   *method, *url, *body;
    size_t  body_len;
    HeaderList headers;
} g_owed[WPT_OWED_MAX];
static int g_owed_n;

/* The document address of the file being run, which is what a relative fetch resolves against. */
static char g_base_url[512];

static void wpt_owed_forget(int i)
{
    free(g_owed[i].url);
    free(g_owed[i].method);
    free(g_owed[i].body);
    header_list_free(&g_owed[i].headers);
    g_owed[i] = g_owed[--g_owed_n];
}

static void wpt_owe(JSContext *ctx, JSValueConst deliver, JSValueConst value, const FetchRequest *req)
{
    /* Both halves are the component's to state and this host fills in neither: a default method would be a real
       value belonging to some other request, and an empty URL would park a flow on an address no host answers. */
    const char *method = req->method;
    const char *url = req->url;
    int i;

    DCHECK(method && *method && url && *url,
           "the wpt provider was owed a request that does not state its METHOD and URL — the reply seam is "
           "keyed on the pair (solver/engine.h), so a request missing either cannot be listed, fetched or "
           "delivered. The component that built it must state both");

    for (i = 0; i < g_owed_n; i++) {
        /* Keyed like the seam, on the pair: a GET and a POST to one address are two lines on the join and two
           deliveries at the provide. */
        if (strcmp(g_owed[i].url, url) || strcmp(g_owed[i].method, method)) continue;
        /* The same request from another flow is ordinary: a candidate re-fire repeats the exploring flow's
           fetches, and the frontier answers all of them with one reply. A different body under one (method, url)
           is the residual above, and it is fatal rather than first come, first served. */
        DCHECK(g_owed[i].body_len == req->body_len &&
               (req->body_len == 0 || (g_owed[i].body && req->body &&
                                       !memcmp(g_owed[i].body, req->body, req->body_len))),
               "two outstanding requests share one (method, url) and differ in BODY — the reply seam names the "
               "pair and not the body, so whichever is answered first answers both and one flow resumes with "
               "the reply to a question it never asked. Put the whole request's identity on the line");
        /* Park the flow on its own register, as the extension does. */
        engine_pending_fetch_url(ctx, deliver, value, req);
        return;
    }
    CHECK(g_owed_n < WPT_OWED_MAX, "wpt: more replies owed at once than this runner tracks");
    g_owed[g_owed_n].method = strdup(method);
    g_owed[g_owed_n].url = strdup(url);
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
    engine_pending_fetch_url(ctx, deliver, value, req);
}

/* The corpus is served by WPT's own server over a socket (engine/wptserve.py), as a browser gets it. A `.py` path
   is a wptserve handler that the server imports and calls, so reading the file off disk would answer with the
   handler's source. Serving everything through it makes the rewrites, directory listings and content types
   wptserve's own. The Host header carries the URL's own authority while the socket goes to the loopback port, as
   a hosts-file mapping does for a real browser. */
static char g_server[64];   /* "127.0.0.1:PORT", from the driver */
/* The origin the corpus is served as, a separate fact from the address above. The top-level document is installed
   with it, this host stamps it on a message it routes out of that document, and every test resolves its own URLs
   against it. It varies per run: engine/wptserve.py reserves an ephemeral port, and wptserve's `sub` pipe
   substitutes `{{host}}` and `{{ports[http][0]}}` into every `.sub` document from the server's config. So the
   origin comes from the server (WPT_TOP_ORIGIN in the environment, beside WPT_SERVER), and a run that cannot
   establish it has no literal to fall back to. */
static char g_top_origin[128];

static void wpt_send_all(int fd, const char *p, size_t n)
{
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w <= 0) return;
        p += w; n -= (size_t)w;
    }
}

/* What this host computed the resource to be (core/fetch/fetch.h). This runner is the corpus's network side, holding
   the socket and the bytes, so it is the zone entitled to answer, as `extension/lib/safe-fetch.js` is in the
   extension. The answer is §4.2's essence of the `Content-Type` wptserve sent; a response that named none answers
   the empty string, §5.1's "the supplied MIME type is undefined". Malloc'd; the caller frees. */
static char *wpt_computed_type(const HeaderList *rh)
{
    MimeType m;
    char *ct = header_list_get(rh, "content-type");
    char *out = NULL;

    if (mime_type_extract(&m, ct)) out = mime_type_essence(&m);
    mime_type_free(&m);
    free(ct);
    if (!out) { out = malloc(1); CHECK(out, "wpt: OOM stating a reply's computed type"); out[0] = 0; }
    return out;
}

/* ---- The network edge: a set of requests in flight, never a call that waits ---------------------------
 *
 * A request is begun and this host returns to the loop; its socket is a row in the table below, and the flow that
 * asked stays parked on the engine's register. It is the park the extension pays through engine.h's `qjs_pending`
 * → fetch → `qjs_provide` pair, whose network side is a Promise. Every slice boundary drains what arrived and
 * delivers it. A blocking read would freeze the whole frontier, including the flow whose work may produce the
 * answer. There is no timeout, poll budget or retry count: a reply that never comes leaves its flow parked. With
 * nothing runnable and requests outstanding, this host blocks in `poll` with an infinite timeout.
 *
 * Connections persist and are pooled as a browser's are (RFC 9112 §9.3 "Persistence"; §9.6 "Tear-down" forbids
 * another request after `close`). wptserve is a ThreadingHTTPServer with a stash-manager connection per thread, so
 * one connection per request churns the server. The pool key is the target URI's authority, not the socket
 * address, because every corpus authority resolves to the one loopback port. There is no per-origin limit. The body
 * length is read in RFC 9112 §6.3 "Message Body Length"'s order; rule 8's close-delimited body (how wptserve serves
 * static files) ends the connection, so persistence is a property of each response. */
enum { WPT_REQ_SEND, WPT_REQ_READ, WPT_REQ_DONE };
/* RFC 9112 §6.3 "Message Body Length" as its reader's states: one per way a body can be delimited, plus the ones
   §7.1 "Chunked Transfer Coding" needs. A reader in none of them has not decided the framing. */
enum { WPT_MSG_HEAD, WPT_MSG_LENGTH, WPT_MSG_CHUNK_SIZE, WPT_MSG_CHUNK_DATA, WPT_MSG_CHUNK_CRLF,
       WPT_MSG_TRAILER, WPT_MSG_CLOSE, WPT_MSG_COMPLETE };
/* What a completed request is delivered as. The four kinds differ only in the answer they build and whom they hand
   it to: one HTTP implementation with four consumers. SYNC has no flow behind it: the document this process was
   started to run, and its external scripts, are fetched before there is a session, so nothing parks or starves. */
enum { WPT_DELIVER_SYNC, WPT_DELIVER_FETCH, WPT_DELIVER_DOCUMENT, WPT_DELIVER_XHR };

#define WPT_INFLIGHT_MAX 64
#define WPT_CONN_MAX 64

/* One TCP connection, held open. `owner` is the request on it and 0 when it is idle. This runner does not pipeline
   (RFC 9112 §9.3.2 "Pipelining" permits it and no browser does it), so a connection carries one message at a time
   and §9.2 "Associating a Response to a Request"'s outstanding-request list is one slot. `in` is the octets read
   and not yet consumed by a message. It belongs to the connection because framing does; without pipelining
   nothing should remain in it when a message ends. */
typedef struct {
    uint64_t id;              /* stable across the table's compaction — an index is not */
    char    *authority;       /* the pool key: the target URI's host and port, the Host header's own value */
    int      fd;
    int      connecting;      /* the socket is still completing its connect(2) */
    uint64_t owner;           /* the request occupying it; 0 when idle */
    char    *in;
    size_t   in_cap, in_n;
} WptConn;
static WptConn g_conn[WPT_CONN_MAX];
static int g_conn_n;
static uint64_t g_conn_seq;

typedef struct {
    uint64_t seq;              /* stable across the table's compaction — an index is not */
    int      state, deliver, failed;
    int      cancelled;        /* a `fetch.cancel` notice named it: the bytes finish, the reply reaches nobody */
    char    *method, *url;     /* with `mode`, the key engine_provide delivers on; `url` is what an answer reports */
    char    *mode;             /* Fetch §2.2.5's request mode, the line's own word */
    char    *referrer_policy;  /* …and the line's referrer policy and referrer, the rest of the key */
    char    *referrer;
    char    *authority;        /* which pool this request draws its connection from */
    uint64_t conn;             /* the connection carrying it; 0 before it has one */
    int      reused;           /* it was put on an already-open connection; see §9.3.1 Retrying Requests */
    uint32_t id;               /* DOCUMENT/XHR: the request id engine_host_answer names */
    char    *out;              /* the whole request, serialized once at begin so nothing it borrowed has to
                                  outlive this call: the ownership half of making a call a work item */
    size_t   out_n, out_off;
    /* The reply, parsed as it arrives: a held-open connection has no end of stream, so the header section is
       parsed once it is complete and the body is accumulated by whichever of §6.3's framings it chose. */
    int      msg;              /* WPT_MSG_*: which delimiter this reader is inside */
    int      status;
    int      http_minor;       /* the version, which §9.3 "Persistence" asks about */
    int      persist;          /* may the connection carry another request after this response */
    HeaderList hdrs;
    char    *body;
    size_t   body_n, body_cap;
    size_t   need;             /* LENGTH: octets of the body still to come; CHUNK_DATA: of this chunk */
} WptRequest;
static WptRequest g_inflight[WPT_INFLIGHT_MAX];
static int g_inflight_n;
static uint64_t g_inflight_seq;

static WptRequest *wpt_request_find(uint64_t seq)
{
    int i;
    for (i = 0; i < g_inflight_n; i++) if (g_inflight[i].seq == seq) return &g_inflight[i];
    return NULL;
}

static WptConn *wpt_conn_find(uint64_t id)
{
    int i;
    for (i = 0; i < g_conn_n; i++) if (g_conn[i].id == id) return &g_conn[i];
    return NULL;
}

/* Whether this question is already asked. The pending registers list an entry until it is answered, so a host
   issuing from them at every slice boundary would send a page's one `fetch` once per slice. Two functions because
   the delivery has two keys: a network park is its five-field identity (engine.h) and a host request is its id. */
static bool wpt_request_asked(const char *method, const char *mode, const char *referrer_policy,
                              const char *referrer, const char *url)
{
    int i;
    for (i = 0; i < g_inflight_n; i++)
        if (g_inflight[i].deliver == WPT_DELIVER_FETCH && !g_inflight[i].cancelled &&
            !strcmp(g_inflight[i].method, method) && !strcmp(g_inflight[i].mode, mode) &&
            !strcmp(g_inflight[i].referrer_policy, referrer_policy) &&
            !strcmp(g_inflight[i].referrer, referrer) && !strcmp(g_inflight[i].url, url)) return true;
    return false;
}

static bool wpt_request_asked_id(uint32_t id)
{
    int i;
    for (i = 0; i < g_inflight_n; i++)
        if (g_inflight[i].deliver != WPT_DELIVER_FETCH && g_inflight[i].deliver != WPT_DELIVER_SYNC &&
            !g_inflight[i].cancelled && g_inflight[i].id == id) return true;
    return false;
}

/* Drop one connection and forget its buffered octets. A request still on it is not failed here: the caller that
   killed the connection decides whether this was the end of a message, a network error or a retry. */
static void wpt_conn_release_at(int i)
{
    WptConn *c = &g_conn[i];
    if (c->fd >= 0) close(c->fd);
    free(c->authority);
    free(c->in);
    g_conn[i] = g_conn[--g_conn_n];
}

static void wpt_conn_release(WptConn *c)
{
    int i;
    for (i = 0; i < g_conn_n; i++) if (&g_conn[i] == c) { wpt_conn_release_at(i); return; }
    DFAIL("a connection was released that this runner's pool does not hold — the pool is the only owner of a "
          "corpus socket, so a pointer into it that no slot matches is a pointer to a freed slot");
}

/* A connection for this authority. An idle open one is reused; `force_new` is asked only by §9.3.1's retry, where
   reuse is what just failed. */
static WptConn *wpt_conn_acquire(const char *authority, int force_new, int *preused)
{
    struct sockaddr_in a;
    char host[64];
    const char *colon = strchr(g_server, ':');
    WptConn *c;
    int fd, i;

    CHECK(colon != NULL, "wpt: the driver did not name a server to serve the corpus from");
    CHECK((size_t)(colon - g_server) < sizeof host, "wpt: the server address is longer than this runner holds");

    if (!force_new)
        for (i = 0; i < g_conn_n; i++)
            if (!g_conn[i].owner && !g_conn[i].connecting && !strcmp(g_conn[i].authority, authority)) {
                *preused = 1;
                return &g_conn[i];
            }

    /* A full pool gives up an idle connection, which is a cache entry: nothing is queued, dropped or deferred, and
       the next request to its authority pays a connect. A connection carrying a request cannot be given up, so the
       CHECK below stays fatal. */
    if (g_conn_n >= WPT_CONN_MAX)
        for (i = 0; i < g_conn_n; i++)
            if (!g_conn[i].owner) { wpt_conn_release_at(i); break; }
    CHECK(g_conn_n < WPT_CONN_MAX, "wpt: more corpus connections carrying a request at once than this runner "
                                   "tracks — every slot holds a request that has been sent and not answered");
    fd = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(fd >= 0, "wpt: no socket to reach the corpus server");
    CHECK(fcntl(fd, F_SETFL, O_NONBLOCK) == 0, "wpt: a corpus connection could not be made non-blocking — the "
                                               "whole frontier waits behind a socket that can block");
    memcpy(host, g_server, (size_t)(colon - g_server));
    host[colon - g_server] = 0;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)atoi(colon + 1));
    a.sin_addr.s_addr = inet_addr(host);

    c = &g_conn[g_conn_n++];
    memset(c, 0, sizeof *c);
    c->id = ++g_conn_seq;
    c->authority = strdup(authority);
    CHECK(c->authority != NULL, "wpt: OOM recording a corpus connection");
    c->fd = fd;
    c->connecting = 1;
    if (connect(fd, (struct sockaddr *)&a, sizeof a) == 0) c->connecting = 0;
    else if (errno != EINPROGRESS && errno != EINTR) { wpt_conn_release_at(g_conn_n - 1); *preused = 0; return NULL; }
    *preused = 0;
    return c;
}

/* Consume the octets a message has finished with, so what is left in the connection's buffer is always the
   part of the stream nothing has read yet. */
static void wpt_conn_consume(WptConn *c, size_t n)
{
    DCHECK(n <= c->in_n, "a message consumed more octets from a corpus connection than the connection had "
                         "read — the reader's own arithmetic decides where a body ends, so a consume past the "
                         "end is a length this file computed and not one the server sent");
    memmove(c->in, c->in + n, c->in_n - n);
    c->in_n -= n;
}

/* Put a request on a connection and start it sending. */
static void wpt_request_attach(WptRequest *r, int force_new)
{
    WptConn *c = wpt_conn_acquire(r->authority, force_new, &r->reused);
    if (!c) { r->failed = 1; r->state = WPT_REQ_DONE; return; }
    DCHECK(c->owner == 0, "a request was attached to a corpus connection another request is still on — this "
                          "runner does not pipeline, so one connection carries one message at a time and the "
                          "second reply would be read as the first one's body");
    c->owner = r->seq;
    r->conn = c->id;
    r->state = WPT_REQ_SEND;
    r->out_off = 0;
    r->msg = WPT_MSG_HEAD;
}

/* Begin one request against the corpus server and return, whatever stage it reaches. The socket is non-blocking
   from before the connect, so no stage below can hold this thread. */
static uint64_t wpt_request_begin(const FetchRequest *req, int deliver, uint32_t id)
{
    UrlRecord base, rec;
    char *path = NULL, *authority = NULL;
    WptRequest *r;
    size_t hn, n;
    int i;

    CHECK(g_inflight_n < WPT_INFLIGHT_MAX, "wpt: more requests in flight at once than this runner tracks");

    r = &g_inflight[g_inflight_n++];
    memset(r, 0, sizeof *r);
    r->seq = ++g_inflight_seq;
    r->deliver = deliver;
    r->id = id;
    r->method = strdup(req->method ? req->method : "GET");
    r->url = strdup(req->url ? req->url : "");
    /* A host request (a document, an XHR answered by id) is not keyed on the mode, and states none. */
    r->mode = strdup(req->mode != FETCH_MODE_UNPLACED ? fetch_mode_token(req->mode) : "");
    r->referrer_policy = strdup(req->referrer_policy ? req->referrer_policy : "");
    r->referrer = strdup(req->referrer ? req->referrer : "");
    CHECK(r->method && r->url && r->mode && r->referrer_policy && r->referrer,
          "wpt: OOM recording a request in flight");

    url_record_init(&base);
    url_record_init(&rec);
    if (url_parse(&base, g_base_url, strlen(g_base_url), NULL) &&
        url_parse(&rec, req->url, strlen(req->url), &base)) {
        /* The request target is the path and the query. The fragment never goes on the wire, which is what
           `exclude_fragment` says, and the authority travels in the Host header. */
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
    /* An address that does not parse is a network error and is delivered as one: the flow that asked is parked on
       this pair, so a silent return would park it for the life of the run. */
    if (!path) { free(authority); r->failed = 1; r->state = WPT_REQ_DONE; return r->seq; }
    r->authority = strdup(authority ? authority : "127.0.0.1");
    CHECK(r->authority != NULL, "wpt: OOM recording a request's authority");

    hn = strlen(path) + strlen(r->authority) + 512
       + (req->headers ? (size_t)req->headers->n * 512 : 0) + (req->body ? req->body_len : 0);
    r->out = malloc(hn);
    CHECK(r->out, "wpt: OOM building a request");
    /* No `Connection` field, which is RFC 9112 §9.3 "Persistence"'s default for HTTP/1.1: `close` would forbid a
       second request on this connection (§9.6 "Tear-down"), and `keep-alive` is HTTP/1.0's way of asking for what
       1.1 already gives. */
    n = (size_t)snprintf(r->out, hn, "%s %s HTTP/1.1\r\nHost: %s\r\n",
                         req->method ? req->method : "GET", path, r->authority);
    for (i = 0; req->headers && i < req->headers->n; i++)
        n += (size_t)snprintf(r->out + n, hn - n, "%s: %s\r\n",
                              req->headers->e[i].name, req->headers->e[i].value);
    n += (size_t)snprintf(r->out + n, hn - n, "Content-Length: %zu\r\n\r\n", req->body ? req->body_len : 0);
    /* `snprintf` answers the length it would have written, so an accumulated `n` past the buffer's end would make
       every `hn - n` below underflow. The sizing above is generous; this covers a header list that outgrows it. */
    CHECK(n < hn, "wpt: a request outgrew the buffer sized for it — the header list is larger than the "
                  "allowance above, and the writes past this point would be counted against a negative size");
    if (req->body && req->body_len) { memcpy(r->out + n, req->body, req->body_len); n += req->body_len; }
    r->out_n = n;
    free(path);
    free(authority);

    wpt_request_attach(r, /*force_new*/0);
    return r->seq;
}

/* ---- Reading one response: RFC 9112 §6.3 "Message Body Length" ------------------------------------------ */

/* Does a comma-separated field value carry this connection option? RFC 9112 §9.3 "Persistence" and §9.6
   "Tear-down" both ask that of `Connection`, case-insensitively and per token. */
static bool wpt_field_has_token(const char *value, const char *token)
{
    size_t tn = strlen(token);
    const char *p = value;

    if (!value) return false;
    while (*p) {
        const char *end;
        while (*p == ' ' || *p == '\t' || *p == ',') p++;
        end = p;
        while (*end && *end != ',') end++;
        {
            const char *stop = end;
            size_t i;
            while (stop > p && (stop[-1] == ' ' || stop[-1] == '\t')) stop--;
            if ((size_t)(stop - p) == tn) {
                for (i = 0; i < tn; i++) {
                    char a = p[i], b = token[i];
                    if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
                    if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
                    if (a != b) break;
                }
                if (i == tn) return true;
            }
        }
        p = *end ? end + 1 : end;
    }
    return false;
}

/* A body of zero octets is a body. A reply becomes `fetch`'s network error by having no body (wpt_reply_value
   answers JS_NULL for one), so a 200 with `Content-Length: 0`, which many of the corpus's `.py` handlers answer,
   must arrive with an empty buffer rather than a null one. Called once the status line has been accepted, the first
   moment there is a response. */
static void wpt_body_init(WptRequest *r)
{
    if (r->body) return;
    r->body = malloc(1);
    CHECK(r->body, "wpt: OOM opening a reply body");
    r->body[0] = 0;
    r->body_cap = 1;
    r->body_n = 0;
}

static void wpt_body_append(WptRequest *r, const char *p, size_t n)
{
    if (r->body_n + n + 1 > r->body_cap) {
        size_t want = r->body_cap ? r->body_cap : 1024;
        while (want < r->body_n + n + 1) want *= 2;
        r->body = realloc(r->body, want);
        CHECK(r->body, "wpt: OOM growing a reply body");
        r->body_cap = want;
    }
    memcpy(r->body + r->body_n, p, n);
    r->body_n += n;
    r->body[r->body_n] = 0;
}

/* The header section, then §6.3's order of precedence over it. `head` is the whole section including its
   terminating CRLF. Answers whether the reader may continue; a false answer has already failed the request. */
static bool wpt_message_head(WptRequest *r, char *head, size_t hn)
{
    char *line, *sep = head + hn - 4;
    char *cl, *te, *conn;

    /* The status line. An unrecognised version means the octets on this connection are not an HTTP/1 message,
       and nothing below can be computed from them. */
    if (hn < 12 || memcmp(head, "HTTP/1.", 7) != 0) { r->failed = 1; return false; }
    r->http_minor = head[7] - '0';
    r->status = atoi(head + 9);
    wpt_body_init(r);

    header_list_free(&r->hdrs);   /* zeroes it — a 1xx below comes back here for the final response */
    /* Every field line, including the last. The section ends with "\r\n\r\n", which is the last field's own CRLF
       followed by the empty line, so the search for a line's CR must include `sep` itself: the range is
       [line+1, sep] inclusive, `sep - line` octets. Dropping the last field loses whatever it carried, a
       Content-Type or a Content-Length, and a lost Content-Length frames the body by close (§6.3 rule 8) on a
       connection the server holds open. */
    line = memchr(head, '\n', hn);
    while (line && line + 1 < sep) {
        char *end = memchr(line + 1, '\r', (size_t)(sep - line));
        char *colon;
        if (!end) break;
        *end = 0;
        colon = strchr(line + 1, ':');
        if (colon) {
            char *v = colon + 1;
            *colon = 0;
            while (*v == ' ' || *v == '\t') v++;
            header_list_append(&r->hdrs, line + 1, v);
        }
        line = end + 1;
    }

    /* §9.3 "Persistence": the `close` option ends it, an HTTP/1.0 response ends it unless it asked for
       `keep-alive`, and otherwise an HTTP/1.1 response persists. Rule 8 below overrides this to 0. */
    conn = header_list_get(&r->hdrs, "connection");
    r->persist = !wpt_field_has_token(conn, "close") &&
                 (r->http_minor >= 1 || wpt_field_has_token(conn, "keep-alive"));
    free(conn);

    /* §6.3 rule 1: a HEAD response and a 1xx/204/304 response end at the first empty line after the header fields,
       regardless of the fields present. wptserve answers a HEAD with its GET's `Content-Length` and no body, so a
       reader that believed the field would wait for ever on a connection that stays open. */
    if (!strcmp(r->method, "HEAD") || r->status == 204 || r->status == 304) {
        r->msg = WPT_MSG_COMPLETE;
        return true;
    }
    /* §9.2 "Associating a Response to a Request": informational responses may precede the final one. A 1xx has no
       body (rule 1), so the reader goes back for the response the request is waiting on. */
    if (r->status >= 100 && r->status < 200) {
        r->msg = WPT_MSG_HEAD;
        return true;
    }
    /* §6.3 rule 2 is the CONNECT tunnel. This runner speaks to the corpus server directly and issues no CONNECT,
       so reaching it would mean the request record and the wire disagree about the method. */
    DCHECK(strcmp(r->method, "CONNECT") != 0,
           "a CONNECT reply reached this runner's message reader — RFC 9112 §6.3 Message Body Length rule 2 "
           "makes the octets after such a response a TUNNEL rather than a body, and this host has no proxy to "
           "tunnel through, so the request that produced it was built by something that must not have");

    te = header_list_get(&r->hdrs, "transfer-encoding");
    cl = header_list_get(&r->hdrs, "content-length");
    if (te) {
        /* §6.3 rule 3: Transfer-Encoding overrides Content-Length, and a message carrying both "ought to be
           handled as an error". It is one here: the corpus server sends one or the other. */
        if (cl) { free(te); free(cl); r->failed = 1; return false; }
        /* §6.3 rule 4 with §7.1 "Chunked Transfer Coding". The octets of a coding this host cannot decode are the
           coded form, so it crashes at the read rather than delivering a document that parses and is not what the
           server sent. */
        if (!wpt_field_has_token(te, "chunked") || strchr(te, ',')) {
            free(te);
            DFAIL("a corpus reply named a transfer coding this host does not decode — RFC 9112 §6.3 Message "
                  "Body Length rule 4 frames the body by that coding, so the octets after the header section "
                  "are the CODED form and handing them on would be a short document that parses; build the "
                  "coding in this file's message reader (§7.2 Transfer Codings for Compression names them)");
            /* In a release build the line above is nothing, so the coding is a network error, the answer §6.3
               rule 5 gives for framing that cannot be determined, never the coded octets handed on as content. */
            r->failed = 1;
            return false;
        }
        free(te);
        r->msg = WPT_MSG_CHUNK_SIZE;
        return true;
    }
    if (cl) {
        /* §6.3 rule 6: a valid Content-Length is the body length. Rule 5 makes an invalid one an unrecoverable
           framing error at which a user agent must close the connection and discard the response, which is the
           network error this delivers, never a body cut where the digits stopped. Rule 5's one exception is a
           comma-separated list of identical valid values, the shape a field sent twice arrives in
           (header_list_get joins repeats with ", "). */
        const char *p = cl;
        unsigned long long v = 0;
        int first = 1, bad = 0;
        for (;;) {
            unsigned long long w = 0;
            const char *d;
            while (*p == ' ' || *p == '\t') p++;
            d = p;
            while (*p >= '0' && *p <= '9') {
                /* A length this machine cannot represent is not a length. §7.1 says the same of a chunk size:
                   anticipate large numerals and do not let the conversion overflow into a small one. */
                if (w > ((unsigned long long)-1 - 9) / 10) { bad = 1; break; }
                w = w * 10 + (unsigned long long)(*p - '0');
                p++;
            }
            if (bad || p == d) { bad = 1; break; }
            while (*p == ' ' || *p == '\t') p++;
            if (!first && w != v) { bad = 1; break; }
            v = w;
            first = 0;
            if (!*p) break;
            if (*p != ',') { bad = 1; break; }
            p++;
        }
        if (!bad && (unsigned long long)(size_t)v != v) bad = 1;   /* wider than this machine's size_t */
        free(cl);
        if (bad) { r->failed = 1; return false; }
        r->need = (size_t)v;
        r->msg = r->need ? WPT_MSG_LENGTH : WPT_MSG_COMPLETE;
        return true;
    }
    /* §6.3 rule 8: a response that declares neither is delimited by the server closing the connection, so this
       connection cannot carry another request. wptserve serves every static file this way. */
    r->msg = WPT_MSG_CLOSE;
    r->persist = 0;
    return true;
}

/* Consume as much of the connection's buffer as this message can, and no more. */
static void wpt_message_feed(WptRequest *r, WptConn *c)
{
    for (;;) {
        if (r->failed || r->msg == WPT_MSG_COMPLETE) return;
        if (r->msg == WPT_MSG_HEAD) {
            char *sep = c->in_n ? memmem(c->in, c->in_n, "\r\n\r\n", 4) : NULL;
            size_t hn;
            if (!sep) return;
            hn = (size_t)(sep - c->in) + 4;
            if (!wpt_message_head(r, c->in, hn)) return;
            wpt_conn_consume(c, hn);
            continue;
        }
        if (r->msg == WPT_MSG_LENGTH) {
            size_t take = c->in_n < r->need ? c->in_n : r->need;
            if (!take) return;
            wpt_body_append(r, c->in, take);
            wpt_conn_consume(c, take);
            r->need -= take;
            if (!r->need) r->msg = WPT_MSG_COMPLETE;
            continue;
        }
        if (r->msg == WPT_MSG_CLOSE) {
            if (!c->in_n) return;
            wpt_body_append(r, c->in, c->in_n);
            wpt_conn_consume(c, c->in_n);
            continue;
        }
        if (r->msg == WPT_MSG_CHUNK_SIZE) {
            /* §7.1.3 "Decoding Chunked": chunk-size is 1*HEXDIG, then an optional chunk-ext (§7.1.1, which a
               recipient must ignore) and CRLF. A size that is not hex is a decoding failure, which §8 "Handling
               Incomplete Messages" makes an incomplete message and so a network error;
               `fetch/api/resources/bad-chunk-encoding.py` sends one on purpose. */
            char *nl = c->in_n ? memchr(c->in, '\n', c->in_n) : NULL;
            size_t i = 0, ln;
            unsigned long long v = 0;
            if (!nl) return;
            ln = (size_t)(nl - c->in) + 1;
            while (i < ln && ((c->in[i] >= '0' && c->in[i] <= '9') ||
                              (c->in[i] >= 'a' && c->in[i] <= 'f') ||
                              (c->in[i] >= 'A' && c->in[i] <= 'F'))) {
                char d = c->in[i];
                /* §7.1: "recipients MUST anticipate potentially large hexadecimal numerals and prevent parsing
                   errors due to integer conversion overflows". An overflowed size would frame a later part of the
                   stream as this chunk's data, the smuggling shape §11.2 warns about. */
                if (v > ((unsigned long long)-1 - 15) / 16) { r->failed = 1; return; }
                v = v * 16 + (unsigned long long)(d <= '9' ? d - '0' : (d | 0x20) - 'a' + 10);
                i++;
            }
            if (i == 0 || (unsigned long long)(size_t)v != v) { r->failed = 1; return; }
            r->need = (size_t)v;
            wpt_conn_consume(c, ln);
            r->msg = r->need ? WPT_MSG_CHUNK_DATA : WPT_MSG_TRAILER;
            continue;
        }
        if (r->msg == WPT_MSG_CHUNK_DATA) {
            size_t take = c->in_n < r->need ? c->in_n : r->need;
            if (!take) return;
            wpt_body_append(r, c->in, take);
            wpt_conn_consume(c, take);
            r->need -= take;
            if (!r->need) r->msg = WPT_MSG_CHUNK_CRLF;
            continue;
        }
        if (r->msg == WPT_MSG_CHUNK_CRLF) {
            if (c->in_n < 2) return;
            if (c->in[0] != '\r' || c->in[1] != '\n') { r->failed = 1; return; }
            wpt_conn_consume(c, 2);
            r->msg = WPT_MSG_CHUNK_SIZE;
            continue;
        }
        if (r->msg == WPT_MSG_TRAILER) {
            /* §7.1.2 "Chunked Trailer Section", ended by the empty line §7.1's chunked-body ends with. A recipient
               may discard the trailer fields and this one does; §7.1.2 forbids merging one into the header section
               unless that field's definition says how. */
            char *nl = c->in_n ? memchr(c->in, '\n', c->in_n) : NULL;
            size_t ln;
            if (!nl) return;
            ln = (size_t)(nl - c->in) + 1;
            if (ln <= 2) r->msg = WPT_MSG_COMPLETE;
            wpt_conn_consume(c, ln);
            continue;
        }
        DFAIL("this runner's message reader is in a state RFC 9112 §6.3 Message Body Length does not define — "
              "every framing is one of the states above and a reader outside them has decided nothing");
    }
}

/* The peer hung up. §6.3 rule 8's framing ends here; for every other framing this is §8 "Handling Incomplete
   Messages": a body shorter than its Content-Length, or a chunked stream with no zero-sized chunk, is incomplete
   and is delivered as a network error, never as the short body that arrived. */
static void wpt_message_eof(WptRequest *r)
{
    if (r->msg == WPT_MSG_CLOSE) { r->msg = WPT_MSG_COMPLETE; return; }
    if (r->msg != WPT_MSG_COMPLETE) r->failed = 1;
}

/* Move one connection, and the request on it, as far as they can go without blocking. */
static void wpt_conn_advance(WptConn *c)
{
    WptRequest *r = c->owner ? wpt_request_find(c->owner) : NULL;

    if (c->connecting) {
        int err = 0;
        socklen_t el = sizeof err;
        if (getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &err, &el) < 0 || err) {
            if (r) { r->failed = 1; r->state = WPT_REQ_DONE; r->conn = 0; }
            wpt_conn_release(c);
            return;
        }
        c->connecting = 0;
    }
    if (!r) {
        /* An idle connection with something to say. §9.2 "Associating a Response to a Request": data on a
           connection with no outstanding request is not a response, and the client closes. This is normally the
           server hanging up after a close-delimited response; dropping the connection here keeps a stale one from
           being handed to the next request. */
        char probe[512];
        ssize_t got = read(c->fd, probe, sizeof probe);
        if (got < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) return;
        DCHECK(got <= 0, "the corpus server sent octets on a connection with no request outstanding — this "
                         "runner does not pipeline, so those octets are either a reply whose length the "
                         "reader above got wrong or a second reply to one request");
        wpt_conn_release(c);
        return;
    }
    if (r->state == WPT_REQ_SEND) {
        while (r->out_off < r->out_n) {
            ssize_t w = write(c->fd, r->out + r->out_off, r->out_n - r->out_off);
            if (w < 0 && errno == EINTR) continue;
            if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
            if (w <= 0) {
                /* §9.3.1 "Retrying Requests": a connection can close at any time, and a reused one that died
                   before carrying any of this response died for a reason unrelated to this request. It is re-sent
                   on a new connection, so the condition cannot hold twice; it is a state, not a retry count. */
                c->owner = 0;
                r->conn = 0;
                wpt_conn_release(c);
                if (r->reused) { wpt_request_attach(r, /*force_new*/1); return; }
                r->failed = 1;
                r->state = WPT_REQ_DONE;
                return;
            }
            r->out_off += (size_t)w;
        }
        r->state = WPT_REQ_READ;
    }
    if (r->state == WPT_REQ_READ) {
        for (;;) {
            ssize_t got;
            if (c->in_n + 65536 > c->in_cap) {
                size_t want = c->in_cap ? c->in_cap * 2 : (size_t)1 << 17;
                char *g = realloc(c->in, want);
                CHECK(g, "wpt: OOM growing a reply");
                c->in = g;
                c->in_cap = want;
            }
            got = read(c->fd, c->in + c->in_n, c->in_cap - c->in_n);
            if (got < 0 && errno == EINTR) continue;
            if (got < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
            if (got > 0) {
                c->in_n += (size_t)got;
                wpt_message_feed(r, c);
                if (r->failed || r->msg == WPT_MSG_COMPLETE) break;
                continue;
            }
            /* EOF, or an error which is the same end with no octets. */
            if (got < 0) r->failed = 1;
            else wpt_message_eof(r);
            if (r->failed && r->reused && r->status == 0 && c->in_n == 0) {
                /* §9.3.1 again, at the other end: the reused connection closed before one octet of the response
                   arrived, so nothing about this request has been answered yet. */
                r->failed = 0;
                c->owner = 0;
                r->conn = 0;
                wpt_conn_release(c);
                wpt_request_attach(r, /*force_new*/1);
                return;
            }
            r->state = WPT_REQ_DONE;
            c->owner = 0;
            r->conn = 0;
            wpt_conn_release(c);
            return;
        }
        /* The message ended with the connection still open, as §9.3's persistence intends. A framing error takes
           the connection with it: RFC 9112 §6.3 "Message Body Length" rule 5 and §8 "Handling Incomplete
           Messages" both have the recipient close, because after a message whose end nobody could find, every
           later octet on it is unattributable. */
        r->state = WPT_REQ_DONE;
        c->owner = 0;
        r->conn = 0;
        if (r->failed || !r->persist || c->in_n) {
            /* Residue is discarded with the connection. Without pipelining it belongs to no request, and RFC 9112
               §6.3's closing paragraph anticipates it: "a user agent MAY discard the remaining data ... A client
               MUST NOT process, cache, or forward such extra data as a separate response." The corpus produces it:
               `xhr/resources/redirect.py` answers `?code=304` with `Content-Length: 4` and `TEST`, which §6.3 rule
               1 says a 304 cannot carry, because wptserve's `write_content` suppresses a body only for HEAD. No
               client can guarantee its absence, so it is not asserted. */
            wpt_conn_release(c);
            return;
        }
    }
}

static void wpt_request_release(int i)
{
    WptRequest *r = &g_inflight[i];
    WptConn *c = r->conn ? wpt_conn_find(r->conn) : NULL;

    if (c) { c->owner = 0; wpt_conn_release(c); }
    free(r->method); free(r->mode); free(r->referrer_policy); free(r->referrer); free(r->url); free(r->authority); free(r->out); free(r->body);
    header_list_free(&r->hdrs);
    g_inflight[i] = g_inflight[--g_inflight_n];
}

/* The requests still outstanding when the run ends (each one a flow that parked and never resumed, which the
   frontier's own teardown asserts about from the other end), then the connections they held open. Released here
   because a socket is not in the runtime's heap and no GC walk can see it. */
static void wpt_net_free(void)
{
    while (g_inflight_n) wpt_request_release(g_inflight_n - 1);
    while (g_conn_n) wpt_conn_release_at(g_conn_n - 1);
}

/* §4.1's clone of the request's URL list. This runner serves the checked-out corpus and follows no redirect, so
   the list is the one URL it was asked for, which makes `response.url` the address the test fetched and
   `response.redirected` false, both computed rather than declared. */
static JSValue wpt_reply_value(JSContext *ctx, WptRequest *r, int status, HeaderList *rh,
                               char *body, size_t len)
{
    char *cty = wpt_computed_type(rh);
    const char *url = r->url;
    JSValue v = body ? fetch_reply_new(ctx, status, "", rh, body, len, &url, 1, cty, "basic") : JS_NULL;
    free(cty);
    return v;
}

/* One completed request, handed to whoever parked on it. Answers how many registers it filled, which tells a stall
   this host answered from one it could not. */
static int wpt_request_finish(JSContext *ctx, WptRequest *r)
{
    size_t len = r->failed ? 0 : r->body_n;
    char *body = r->failed ? NULL : r->body;
    int status = r->failed ? 0 : r->status;
    int filled = 0, i;

    DCHECK(ctx != NULL, "a reply completed for a flow before this host had a realm to deliver it into — only "
                        "the pre-session loads run without one, and none of them is delivered");
    /* A cancelled send's reply never reaches the engine: no flow is owed it (engine_pending_fetch_cancel). Its
       request record is released here, as an answered one's is below. */
    if (r->cancelled) {
        for (i = 0; i < g_owed_n; i++)
            if (!strcmp(g_owed[i].url, r->url) && !strcmp(g_owed[i].method, r->method)) { wpt_owed_forget(i); break; }
        return 0;
    }
    /* A network error has no header list. What is on the request now is whatever arrived before the framing
       failed, and §2.2.5's network error is a response with none; half a header section beside a null body would
       be two answers about one reply. */
    if (r->failed) header_list_free(&r->hdrs);
    /* The framing must be determined where it is read. A reader that did not reach COMPLETE has not been told by
       RFC 9112 §6.3 Message Body Length where this body ends, so the octets below are a prefix of one, and a
       prefix of a document parses. */
    DCHECK(r->failed || r->msg == WPT_MSG_COMPLETE,
           "a corpus reply is being delivered by a reader that never reached the end of its message — §6.3 "
           "Message Body Length decides where a body ends and this one is a prefix, which parses as a short "
           "document rather than reporting anything");
    if (r->deliver == WPT_DELIVER_DOCUMENT) {
        /* The answer is `{url, body, headers}`: a navigated Document is created from this response, so everything
           §7.5.1 reads off a header list belongs in it. The block is the field-line form qjs_init takes, serialized
           by the component that parses it so the two cannot disagree. §2.2.5's body is a byte sequence and crosses
           as one; `JS_NewStringLen` would run quickjs's UTF-8 decode on bytes HTML's encoding sniffing has not
           seen. `url` is Fetch §2.2.6 "Responses"' response URL, the last of its URL list, which is the address
           requested because this runner follows no redirect (as wpt_reply_value says). HTML §7.4.5 determines a
           navigation's origin over it, so a host that follows redirects reports a different string. It is stated
           even when the fetch failed and `body` is null: §7.4.5 still creates a Document for a load that did not
           load. */
        JSValue v = JS_NewObject(ctx);
        char *field_lines = header_list_field_lines(&r->hdrs);
        DCHECK(r->url != NULL && *r->url,
               "a corpus document load completed with no request URL on its record — this runner writes the "
               "address onto the request when it begins one, and §7.4.5 determines the loaded Document's origin "
               "over the URL the response came from, which for a host that follows no redirect is that address");
        JS_SetPropertyStr(ctx, v, "url", JS_NewString(ctx, r->url));
        JS_SetPropertyStr(ctx, v, "body",
                          body ? JS_NewArrayBufferCopy(ctx, (const uint8_t *)body, len) : JS_NULL);
        JS_SetPropertyStr(ctx, v, "headers", JS_NewString(ctx, field_lines));
        /* This runner holds no response for a later provision record: a swapped-to or peer document is a child
           instance that loads its address itself. */
        JS_SetPropertyStr(ctx, v, "held", JS_NewString(ctx, "-"));
        free(field_lines);
        /* Each Set-Cookie field beside the URL it answered (Fetch §4.7 "HTTP-network fetch"); this runner follows
           no redirect, so every field answered `r->url`. Only this agent's jar is handed its own origin's pairs:
           a response of another origin is a document a child instance hosts, and that child loads it itself. */
        {
            JSValue cookies = JS_NewArray(ctx);
            uint32_t nc = 0;
            int hi;
            UrlRecord answered;
            bool own;

            url_record_init(&answered);
            own = url_parse(&answered, r->url, strlen(r->url), NULL) &&
                  origin_same_as_url(origin_agent(), &answered);
            url_record_free(&answered);
            for (hi = 0; own && hi < r->hdrs.n; hi++) {
                JSValue pair;
                if (strcasecmp(r->hdrs.e[hi].name, "set-cookie") != 0) continue;
                pair = JS_NewArray(ctx);
                JS_SetPropertyUint32(ctx, pair, 0, JS_NewString(ctx, r->url));
                JS_SetPropertyUint32(ctx, pair, 1, JS_NewString(ctx, r->hdrs.e[hi].value));
                JS_SetPropertyUint32(ctx, cookies, nc++, pair);
            }
            JS_SetPropertyStr(ctx, v, "setCookies", cookies);
        }
        /* The host's own answer, so a normal completion: this zone fetched bytes rather than running a peer's
           program, and a load that did not load is `{body: null}` rather than a throw. */
        engine_host_answer(ctx, r->id, NULL, NULL, v, ENGINE_COMPLETION_NORMAL, ENGINE_ANSWER_HOST);
        JS_FreeValue(ctx, v);
        filled = 1;
    } else {
        JSValue reply = wpt_reply_value(ctx, r, status, &r->hdrs, body, len);
        if (r->deliver == WPT_DELIVER_XHR) {
            /* Also the host's own — XHR §3.5.6's fetch, answered out of the network; a network error is a
               reply that component reads, never a thrown value. */
            engine_host_answer(ctx, r->id, NULL, NULL, reply, ENGINE_COMPLETION_NORMAL, ENGINE_ANSWER_HOST);
            filled = 1;
        } else {
            filled = engine_provide(ctx, r->method, r->mode, r->referrer_policy, r->referrer, r->url, reply);
            /* The caller's half of engine_provide's contract. It tells "answered twice" from "answered for nobody"
               and asserts only the first; the second is the caller's, because only the caller holds the credit
               that can excuse it (engine.h: "it is the CALLER that tells those apart"), as main.c does. An
               unmatched reply is not silent: the record stays outstanding, the join keeps listing it, and
               wpt_issue_pending asks again at the next slice (wpt_request_asked dedups only against requests in
               flight), so it becomes a request loop whose progress keeps the frontier from the stall DCHECK.
               A sale is the other way `filled == 0` happens and is not a bug: the engine sold the waiting flow to
               the cold tier, and its recipe re-issues the request. The take consumes one credit, so it cannot live
               in a DCHECK's condition; the `if` runs in every build and only the message is dev-only. */
            if (filled == 0 && !engine_take_paged_owed())
                DFAILF("a corpus reply was provided for a request no flow is parked on and none was paged "
                       "out — the pair this runner fetched under is not the pair the frontier parked on, so "
                       "the flow that IS parked waits for the rest of the run and this address is re-issued "
                       "at every slice. request=%s %s %s %s %s", r->method, r->mode, r->referrer_policy,
                       r->referrer, r->url);
            /* The request record this answered, released only now: it holds the body and headers of the question,
               and a second flow parking the same pair before the answer landed is what its DCHECK catches. */
            for (i = 0; i < g_owed_n; i++)
                if (!strcmp(g_owed[i].url, r->url) && !strcmp(g_owed[i].method, r->method)) {
                    wpt_owed_forget(i);
                    break;
                }
        }
        JS_FreeValue(ctx, reply);
    }
    return filled;
}

#define WPT_CHILD_MAX 32
/* A child document's channel carries lines nobody asked for only in its boot round, the stretch between its
   spawn and the `S` it writes when its frontier first rests; after that it speaks only to answer an ask
   (wpt_child_ask). These two are that round's half of the pump, defined beside the child table. */
static int wpt_children_booting_fds(struct pollfd *pfd, int *which);
static int wpt_child_take_unasked(int which);

/* The host's half of one slice boundary: move every connection as far as it goes, deliver the requests that
   finished, take what a booting child document wrote (its notices, routed, and its `S`), and answer how many
   registers and routed notices that filled. `block` is asked only where the frontier has nothing runnable and
   this host has nothing else to pay (see the loop in main). It is an infinite poll, never a deadline. */
static int wpt_net_pump(JSContext *ctx, int block)
{
    struct pollfd pfd[WPT_CONN_MAX + WPT_CHILD_MAX];
    uint64_t map[WPT_CONN_MAX];
    int which[WPT_CHILD_MAX];
    int nf = 0, nc, owned = 0, i, delivered = 0;

    for (i = 0; i < g_conn_n; i++) {
        WptRequest *r = g_conn[i].owner ? wpt_request_find(g_conn[i].owner) : NULL;
        pfd[nf].fd = g_conn[i].fd;
        /* What each connection waits for. An idle one is watched for read, because all it can produce is the
           hang-up §9.2 says to close on; never for write, which is always ready and would spin. */
        pfd[nf].events = (short)(g_conn[i].connecting || (r && r->state == WPT_REQ_SEND) ? POLLOUT : POLLIN);
        pfd[nf].revents = 0;
        map[nf++] = g_conn[i].id;
        if (r) owned++;
    }
    nc = wpt_children_booting_fds(pfd + nf, which);
    DCHECK(!block || owned > 0 || nc > 0,
           "this host was asked to WAIT with no request outstanding on any connection and no child document "
           "still in its boot round — a poll with nothing to wait for is a hang, and the caller that asked for "
           "it believes an answer is coming that nobody is going to send");
    if (nf + nc) {
        int rc = poll(pfd, (nfds_t)(nf + nc), block ? -1 : 0);
        CHECK(rc >= 0 || errno == EINTR, "wpt: poll over the corpus connections failed");
        for (i = 0; i < nc; i++)
            if (pfd[nf + i].revents) delivered += wpt_child_take_unasked(which[i]);
        /* By id, not by index: advancing one connection can release it (a hang-up, a §9.3.1 retry), and the pool
           compacts, so a slot may now hold a different connection. */
        for (i = 0; i < nf; i++) {
            WptConn *c;
            if (!pfd[i].revents) continue;
            c = wpt_conn_find(map[i]);
            if (c) wpt_conn_advance(c);
        }
    }
    for (i = 0; i < g_inflight_n; ) {
        if (g_inflight[i].state != WPT_REQ_DONE || g_inflight[i].deliver == WPT_DELIVER_SYNC) { i++; continue; }
        delivered += wpt_request_finish(ctx, &g_inflight[i]);
        wpt_request_release(i);
    }
    return delivered;
}

/* Perform one request and return its body, or NULL: the one wait in this file. It is reachable only before the
   session exists, for the document this process was started to run and its external scripts, when there is no
   flow to park, no frontier to starve and nothing else for this thread to do. It is the same request machine as
   above, entered in "wait for this one" mode. A non-2xx is a body like any other; a NULL body is what the
   delivery turns into `fetch`'s TypeError, the answer a real network error gives. */
static char *wpt_http(const FetchRequest *req, size_t *plen, int *pstatus, HeaderList *phdrs)
{
    uint64_t seq = wpt_request_begin(req, WPT_DELIVER_SYNC, 0);
    WptRequest *r;
    char *body;
    int i;

    while ((r = wpt_request_find(seq)) != NULL && r->state != WPT_REQ_DONE) wpt_net_pump(NULL, /*block*/1);
    r = wpt_request_find(seq);
    DCHECK(r != NULL, "the pre-session load this host was waiting for was released by the pump — a SYNC "
                      "request has no register to deliver into and must be reaped by the caller that made it");
    DCHECK(r->failed || r->msg == WPT_MSG_COMPLETE,
           "a pre-session load is being read out of a reader that never reached the end of its message — the "
           "document or script built from these octets would be a PREFIX of the one the server sent");
    *pstatus = r->failed ? 0 : r->status;
    *plen = r->failed ? 0 : r->body_n;
    body = r->failed ? NULL : r->body;
    if (body) r->body = NULL;                      /* moved to the caller, which frees it */
    if (phdrs && !r->failed) { *phdrs = r->hdrs; memset(&r->hdrs, 0, sizeof r->hdrs); }
    for (i = 0; i < g_inflight_n; i++) if (g_inflight[i].seq == seq) { wpt_request_release(i); break; }
    return body;
}

/* Issue every request the frontier is parked on: the host's half of one slice boundary, the operation the
 * extension's `qjs_pending`/`qjs_provide` pair performs. It does not wait; the answer arrives at wpt_net_pump on
 * whichever later boundary the bytes land, as the extension's reply arrives when its Promise settles. Returns how
 * many requests it started, which is progress and therefore not a stall.
 *
 * It does not enter JS. The resolve capability rides the flow's own pending entry: engine_provide lands the
 * reply there, the flow's next step drains it, and the reaction is enqueued on the flow that issued the request.
 *
 * A line with no record in the table is a request the engine issued for code (an external document script, an
 * injected <script src>, a dynamic `import()`). Its method is on the line like every other, so nothing here needs
 * to know which kind of park a line came from; the missing record means only that there are no headers and no
 * body to re-send. */
static int wpt_issue_pending(void)
{
    char *list, *p;
    int issued = 0;

    if (!*engine_pending_fetches()) return 0;
    /* Copied before it is walked: engine_pending_fetches answers out of one buffer it reuses. */
    list = strdup(engine_pending_fetches());
    CHECK(list != NULL, "wpt: OOM copying the frontier's pending list");
    for (p = list; *p; ) {
        char *end = strchr(p, '\n');
        const char *method, *destination, *initiator, *provenance, *pinned, *driven, *credentials, *mode, *url;
        const char *referrer_policy, *referrer, *askers;
        FetchRequest req = {0};   /* §2.2.5 metadata stated as "nothing yet": these are the runner's own
                                     HTTP client requests and reach no park, so a zero is the named abort a
                                     future reader gets rather than a stack address it would compare bytes at */
        HeaderList none = { 0 };
        int i, rec = -1;

        if (!end) break;
        *end = 0;
        /* Split by the engine's own splitter, so the grammar has one reader. This runner issues every park
           whatever it says about itself, because it serves the checked-out corpus off wptserve's loopback socket:
           there is no third party to spend anything at and no session to spend. Who asked and what the request is
           evidence of are consumed by the zone that talks to the real web (engine/trusted.mjs); what is owed here
           is the vocabulary assert, since a runner reading a field it never checked would see the producer drift
           first. */
        engine_pending_split(p, &method, &destination, &initiator, &provenance, &pinned, &driven, &credentials,
                             &mode, &referrer_policy, &referrer, &askers, &url);
        DCHECK(!strcmp(driven, PENDING_DRIVEN_YES) || !strcmp(driven, PENDING_DRIVEN_NO),
               "the pending join stated a drive mark that is neither token engine.h declares");
        DCHECK(!strcmp(initiator, PENDING_INITIATOR_PARSER) || !strcmp(initiator, PENDING_INITIATOR_SCRIPT),
               "the pending join stated an initiator that is neither token engine.h declares");
        DCHECK(!strcmp(pinned, PENDING_PINNED_YES) || !strcmp(pinned, PENDING_PINNED_NO),
               "the pending join stated a pinned mark that is neither token engine.h declares");
        DCHECK(!strcmp(provenance, PENDING_PROVENANCE_OBSERVED) ||
               !strcmp(provenance, PENDING_PROVENANCE_DERIVED) ||
               !strcmp(provenance, PENDING_PROVENANCE_FORCED),
               "the pending join stated a provenance that is none of the three tokens engine.h declares");
        /* Fetch §2.2.5's credentials mode, asserted the same way through the one mapping that owns the three
           words (core/fetch/fetch.h). `fetch_credentials_of_token` is fatal on a word §2.2.5 does not define, so
           the call is the assert. */
        (void)fetch_credentials_of_token(credentials);
        /* An entry stays listed until it is answered and this host visits the list at every slice, so without
           this the page's one `fetch` would become one request per slice at the server. */
        if (wpt_request_asked(method, mode, referrer_policy, referrer, url)) { p = end + 1; continue; }
        for (i = 0; i < g_owed_n; i++)
            if (!strcmp(g_owed[i].url, url) && !strcmp(g_owed[i].method, method)) { rec = i; break; }
        req.method = method;
        req.url = url;
        /* The destination the line stated, put back on its request. This runner does no CORB (the loopback corpus
           has no cross-origin data body to keep out of a compiler), but the record is core/fetch/fetch.h's request,
           and a field left unwritten is an uninitialised read for the first consumer that grows one. */
        req.destination = destination;
        /* The credentials mode the line stated, for the same reason: its zero is the unplaced not-a-value that
           `fetch_owe` and `pending_park_request` both abort on. */
        req.credentials = fetch_credentials_of_token(credentials);
        /* Fetch §2.2.5's mode, part of the key the reply is delivered on. This runner applies no CORS check (its
           loopback corpus is the test's own origin set); the mode is carried so the delivery names the request the
           frontier parked on. */
        req.mode = fetch_mode_of_token(mode);
        /* The referrer pair, the rest of the key, carried for the mode's reason. */
        req.referrer_policy = referrer_policy;
        req.referrer = referrer;
        /* a listed line's URL is bytes: the engine's join lists no other address class */
        req.address = FETCH_ADDRESS_CONCRETE;
        req.headers = rec >= 0 ? &g_owed[rec].headers : &none;
        req.body = rec >= 0 ? g_owed[rec].body : NULL;
        req.body_len = rec >= 0 ? g_owed[rec].body_len : 0;
        wpt_request_begin(&req, WPT_DELIVER_FETCH, 0);
        issued++;
        p = end + 1;
    }
    free(list);
    return issued;
}

/* ---- A second document is a second process ---------------------------------------------------------------
 *
 * Notices are one-way and are drained here: `navigable.create` announces a document the engine has already named
 * and handed the page a WindowProxy for, so there is nothing to answer. The engine mints its own child document
 * names (world.h), which makes §4.8.5's insertion-step creation synchronous.
 *
 * A navigable the engine announces this way is held by another instance (a same-origin child is a second realm
 * in this agent, built by wpt_child_realm). Here that instance is a child process: the same binary, re-executed
 * with the child's name, address, origin and policy, talking over a pipe. The transport is what has to be
 * tested, because the cross-instance COW delta and the origin stamp ride it.
 *
 * What crosses is text, the constraint of the real seam: a live JSValue crosses no process, wasm instance,
 * session or park. An answer carries its type, because `otherW.length` is a number that a page compares with
 * `===`, and the string "0" would satisfy only a loose check. */
/* `at_rest` is whether the child has written the `S` that ends its boot round. */
typedef struct { char *name; char *origin; int to, from; pid_t pid; int at_rest; } ChildDoc;
static ChildDoc g_children[WPT_CHILD_MAX];
static int g_children_n;
static int wpt_child_marker(const ChildDoc *c, const char *line);
static const char *g_self_exe;   /* argv[0], which is what a child navigable is re-executed from */

static ChildDoc *wpt_child_for(const char *name)
{
    int i;
    for (i = 0; i < g_children_n; i++)
        if (!strcmp(g_children[i].name, name)) return &g_children[i];
    return NULL;
}

/* §7.4/§4.8.5's create, from the host's side: provision an instance for a document the engine has already named
   and handed the page a WindowProxy for. The policy arguments are facts the child process cannot derive.
   `csp` and `csp_self_origin` are one argument in two slots: HTML §7.1.7 "Policy containers"' clone of the
   creator's container, whose CSP list CSP §2.2 "Policies" makes "a struct consisting of policies (a list of
   policies) and a self-origin". §2.2.2 "Parse response's Content Security Policies" states a self-origin from
   outside the policy bytes, and the child's own address is the wrong one for an inherited list (CSP §2.2's note:
   the self-origin exists for documents "that have inherited their policy").
   The four `coep*` slots are that container's §7.1.4 embedder policy item: §7.1.7's clone sets the clone's
   embedder policy to a copy of the source's, so keeping it here would leave the child `unsafe-none`. The two
   values cross as §7.1.4's own tokens, not as enum integers whose ends agree only by declaration order. */
/* `parent` is HTML §7.3.1.3 "Child navigables"' parent navigable, in core/frame/remote_object.h's identity form,
   and `u` (undefined) states that there is none, which §7.1.3.2's swap sends. `container_policy` is §7.3.1.3's
   container's Permissions Policy §9.5 result, serialized, because the element is in this process's tree; the
   swap has no container (§7.3.2.3: "with null, null, and group") and says so in that grammar's word.
   `ancestor_origins` is HTML §3.1.3 "Ancestor origins"' internal ancestor origin objects list for the child's
   Document. Its steps read the parent Document's list, that Document's origin record and the container element,
   and a serialization drops the origin record (§7.1.1 decides an opaque origin by identity, and every opaque
   origin serializes to `null`), so the steps run here. The swap provisions a top-level traversable and states
   "there are no ancestors". `creation_sandbox_flags` is HTML §7.1.5 "Sandboxing"'s set for the navigable, read
   off the embedder element's iframe sandboxing flag set and its node document's active set, crossing in
   core/frame/sandboxing.h's flag names; the swap's set comes from a popup sandboxing flag set nothing filled and
   states the empty set's word. None of these is an item of §7.1.7's container, which is five policies. */
static void wpt_spawn_child(const char *name, const char *url, const char *origin, const char *csp,
                            const char *csp_report_only,
                            const char *csp_self_origin, const char *top_level_url,
                            const char *coep, const char *coep_endpoint,
                            const char *coep_report_only, const char *coep_report_only_endpoint,
                            const char *referrer_policy,
                            const char *parent, const char *container_policy,
                            const char *ancestor_origins, const char *creation_sandbox_flags,
                            const char *opener, const char *arrival)
{
    int down[2], up[2];
    pid_t pid;

    DCHECK(csp != NULL && csp_report_only != NULL,
           "a child document was about to be provisioned with no CSP list text — every caller states both "
           "halves of the container's list, \"\" for a half with no policies");
    /* §7.1.4's four items are stated by every caller, never defaulted here. The two callers are the two
       operations: a `navigable.create` carries the creator's, and §7.1.3.2's swap has no creator and states
       §7.1.4's "a new embedder policy". A `?:` here would make those one answer. */
    DCHECK(coep != NULL && coep_endpoint != NULL && coep_report_only != NULL &&
           coep_report_only_endpoint != NULL,
           "a child document was about to be provisioned with no §7.1.4 EMBEDDER POLICY for its §7.1.7 "
           "container — every container has one, so this is a caller that did not state which of the two "
           "answers applies (the creator's clone, or a new embedder policy where there is no creator)");
    /* §7.1.7's referrer policy item, stated by every caller on the embedder policy's rule; "" is a value. */
    DCHECK(referrer_policy != NULL && fetch_is_referrer_policy(referrer_policy),
           "a child document was about to be provisioned with no Referrer Policy §3 value for its §7.1.7 "
           "container's referrer policy — the creator's clone carries its item and a creatorless swap states "
           "the container's initial default referrer policy");
    /* §7.3.1.3's parent is stated by every caller for the same reason, and has no empty spelling: a navigable
       either has a parent or is a top-level traversable, and both are answers. */
    DCHECK(parent != NULL && *parent,
           "a child document was about to be provisioned with no §7.3.1.3 PARENT NAVIGABLE — the section makes "
           "\"is a child navigable\" mean \"its parent is non-null\", so a process started without the field "
           "would root its document in a navigable that presents as a TOP-LEVEL TRAVERSABLE, and every "
           "algorithm that walks up out of it would find nothing rather than crash");
    /* §3.1.3's list likewise: a Document either has ancestors or is at the top of its tree. */
    DCHECK(ancestor_origins != NULL && *ancestor_origins,
           "a child document was about to be provisioned with no §3.1.3 ANCESTOR ORIGINS — the steps return an "
           "empty list for a Document with no container document and a composed one otherwise, so an absent "
           "field is a caller that did not state which applies, and the child would report itself top-level");
    /* §7.1.5's set likewise: the section's word for "no flags" is what a top-level traversable sends. */
    DCHECK(creation_sandbox_flags != NULL && *creation_sandbox_flags,
           "a child document was about to be provisioned with no §7.1.5 CREATION SANDBOXING FLAG SET — the "
           "empty set is an ANSWER and has a word of its own, so an absent field is a caller that did not "
           "state which applies, and the child process would run the scripts, submit the forms and relax the "
           "`document.domain` its embedder's `sandbox` attribute forbids");
    if (wpt_child_for(name)) return;   /* already provisioned; a notice is not a request */
    CHECK(g_children_n < WPT_CHILD_MAX, "wpt: more child documents at once than this runner tracks");
    /* O_CLOEXEC is required: fork() copies every descriptor, so a second child would inherit this parent's write
       end to the first child's stdin, closing the parent's copy would never give that child EOF, its serve loop
       would never end, and teardown would block in waitpid. dup2 clears the flag on the fd it creates, so the
       child's own 0 and 1 survive the exec and the originals do not. */
    CHECK(pipe2(down, O_CLOEXEC) == 0 && pipe2(up, O_CLOEXEC) == 0,
          "wpt: could not open the pipes to a child document");
    pid = fork();
    CHECK(pid >= 0, "wpt: could not fork a child document");
    if (pid == 0) {
        /* The child. stdin carries the questions and stdout the answers, so stdout is not where test output goes;
           a child document has none, because it runs a page, not a test file. */
        dup2(down[0], 0);
        dup2(up[1], 1);
        close(down[0]); close(down[1]); close(up[0]); close(up[1]);
        /* New arguments are appended, never interleaved, so every position already read keeps its meaning and
           the child's argv indexing has one rule. */
        execl(g_self_exe, g_self_exe, "--document", name, url, origin, csp,
              top_level_url ? top_level_url : "", csp_self_origin ? csp_self_origin : "",
              coep, coep_endpoint, coep_report_only, coep_report_only_endpoint, parent, container_policy,
              ancestor_origins, creation_sandbox_flags, opener, arrival, referrer_policy, csp_report_only,
              (char *)NULL);
        _exit(127);
    }
    close(down[0]); close(up[1]);
    g_children[g_children_n].name = strdup(name);
    /* Its origin, kept because this host stamps it on everything that child emits: the child cannot name its own
       sender any more than the top-level document can. */
    g_children[g_children_n].origin = strdup(origin ? origin : "null");
    CHECK(g_children[g_children_n].name != NULL && g_children[g_children_n].origin != NULL,
          "wpt: OOM recording a child document");
    g_children[g_children_n].to = down[1];
    g_children[g_children_n].from = up[0];
    g_children[g_children_n].pid = pid;
    g_children[g_children_n].at_rest = 0;
    g_children_n++;
}

/* Ask a child document one question and read one answer line; false when the child is gone, a real answer for
   the caller to report rather than a reason to hang. The child's own notices come back up this pipe, prefixed
   `N\t`, before its answer: after its boot round (wpt_child_take_unasked) the transport is one question, one
   answer, and without this relay a message it posts would vanish. Routing one may ask another child, which is
   why this is re-entrant on a different child's pipes. */
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

/* Ask one question and return the answer line, borrowed until the next ask. The buffer grows to the line: an
   encoded string is base64 and an argument list has no length, and a truncated answer would decode as a
   different value with nothing to say so. */
static const char *wpt_child_ask(ChildDoc *c, const char *op)
{
    static char *buf;
    static size_t buf_cap;

    wpt_send_all(c->to, op, strlen(op));
    wpt_send_all(c->to, "\n", 1);
    for (;;) {
        if (!wpt_child_read_line(c, &buf, &buf_cap)) return NULL;
        if (!strncmp(buf, "N\t", 2)) {
            /* A notice the child emitted, routed by the zone that knows where it goes and stamped with the child's
               origin, because the child sent it. */
            char *line = strdup(buf + 2);
            CHECK(line != NULL, "wpt: OOM relaying a child's notice");
            DCHECK(g_wpt_ctx != NULL, "a child relayed a notice before this host had a document to route it "
                                      "into — the top-level realm names where a reply is delivered");
            wpt_route_notice(g_wpt_ctx, line, c->origin);
            free(line);
            continue;
        }
        /* The end of the child's boot round, if this ask was written before it rested. */
        if (!strcmp(buf, "S")) { c->at_rest = 1; continue; }
        if (wpt_child_marker(c, buf)) continue;
        return buf;
    }
}

/* A line of the engine's own census and denomination stream (`@QUANTUM`, `@SWAP`, … — solver/quantum.c and
   engine_census_emit print them on stdout, which is this child's channel). It is neither a notice nor an
   answer, so it is relayed to this process's diagnostics under the child's name. Answers to `true` when the
   line was one. */
static int wpt_child_marker(const ChildDoc *c, const char *line)
{
    if (line[0] != '@') return 0;
    fprintf(stderr, "[child %s] %s\n", c->name, line);
    return 1;
}

static int wpt_children_booting_fds(struct pollfd *pfd, int *which)
{
    int i, n = 0;

    for (i = 0; i < g_children_n; i++) {
        if (g_children[i].at_rest) continue;
        pfd[n].fd = g_children[i].from;
        pfd[n].events = POLLIN;
        pfd[n].revents = 0;
        which[n++] = i;
    }
    return n;
}

/* One line a booting child wrote unasked: a notice, routed as wpt_child_ask routes one and counted as work,
   or its `S`. A child that closes its channel before resting ended its boot round by dying, which no answer
   in this protocol covers. */
static int wpt_child_take_unasked(int which)
{
    static char *buf;
    static size_t buf_cap;
    ChildDoc *c = &g_children[which];
    char *line;

    CHECKF(wpt_child_read_line(c, &buf, &buf_cap),
           "wpt: child document %s closed its channel before its boot round rested", c->name);
    if (!strcmp(buf, "S")) { c->at_rest = 1; return 0; }
    if (wpt_child_marker(c, buf)) return 0;
    CHECKF(!strncmp(buf, "N\t", 2),
           "wpt: child document %s wrote a line nobody asked for that is not a notice: [%.120s]", c->name, buf);
    line = strdup(buf + 2);
    CHECK(line != NULL, "wpt: OOM relaying a child's notice");
    DCHECK(g_wpt_ctx != NULL, "a child relayed a notice before this host had a document to route it into");
    wpt_route_notice(g_wpt_ctx, line, c->origin);
    free(line);
    return 1;
}

/* Whether any child document is still in its boot round, and so may still write a notice unasked. */
static int wpt_children_booting(void)
{
    int i;

    for (i = 0; i < g_children_n; i++)
        if (!g_children[i].at_rest) return 1;
    return 0;
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

/* Defined below, next to the other GETs against the corpus server: §7.4.5's fetch is answered there and is the
   same request every other one is. */
static char *wpt_get_headers(const char *url, size_t *plen, HeaderList *pheaders);

/* The trusted zone's router. Every notice comes through here, the top-level document's own and every one
   relayed up from a child instance, because which instance holds which document is a fact only this process
   knows. `from_origin` is the origin of the instance that emitted the notice, which this host knows and the
   emitting engine may not name for itself; a routed message is stamped with it. `line` is modified in place. */
static void wpt_route_notice(JSContext *ctx, char *line, const char *from_origin);

/* Relay one message to whichever instance holds `doc`, a child or this process's own document. `record` is the
   emitting engine's notice verbatim, so the receiving engine's own entry parses window_message.c's grammar and
   nothing here restates it. The zone routes the record and stamps the origin the emitting engine may not name
   for itself (SECURITY.md), so the origin travels beside the record rather than inside it. */
static void wpt_route_post(JSContext *ctx, const char *doc, const char *record, const char *from_origin)
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
    /* Not a child, so this process's own document: the reply direction, a frame answering its opener. It is
       attached, never performed here. The page's `message` listener lives in the delta of the flow that
       registered it, so a task enqueued against whichever flow is switched in would land where nothing listens.
       engine_route makes the record a work item of every live timeline of the receiving document, each delivering
       under its own delta, as the child half does with the same record; the world vector, target origin and
       payload are fields of a grammar window_message.c and world.c own. */
    DCHECK(!strcmp(doc, "wpt"),
           "a routed message named a document neither this process nor any child of it holds — the "
           "navigable.create notice for it was dropped, or the post outran it");
    engine_route(ctx, record, from_origin);
}

/* A BASE64 field of a split create record (navigable_notice.h), decoded with the engine's codec. The field is
   a C string's bytes, so a decoded NUL is the composer writing something else. The caller frees. */
static char *wpt_create_text(char *const *f, int field)
{
    const char *b64 = f[1 + field];
    size_t len = strlen(b64), cap = JS_Base64DecodedMax(len) + 1, n;
    int err = 0;
    char *out = malloc(cap);

    DCHECK(navigable_create_encoding(field) == NAVIGABLE_NOTICE_BASE64,
           "wpt: a create field was read as BASE64 text and navigable_notice.h declares it a TOKEN");
    CHECK(out != NULL, "wpt: OOM decoding a create notice field");
    n = JS_Base64Decode((uint8_t *)out, cap, b64, len, &err);
    CHECK(err == 0 && n < cap, "wpt: a create notice's BASE64 field is not base64 — the engine composed it with "
                               "JS_Base64Encode, so the record was damaged between the two processes");
    out[n] = 0;
    DCHECK(strlen(out) == n, "wpt: a create notice's decoded CSP text carries a NUL, which no C string held");
    return out;
}

/* Field `id` of a split create or provision record (navigable_notice.h): the operation name is f[0]. */
#define WPT_CREATE(id) f[1 + NAVIGABLE_CREATE_##id]
#define WPT_PROVISION(id) f[1 + NAVIGABLE_PROVISION_##id]
#define WPT_REQUEST(id) f[1 + NAVIGABLE_REQUEST_##id]

static void wpt_route_notice(JSContext *ctx, char *line, const char *from_origin)
{
    /* The create record is the longest this router splits; every other arm reads a prefix of the array. */
    char *f[1 + NAVIGABLE_CREATE_N]; int nf = 0; char *q;
    /* The record the emitting engine wrote, kept whole: the split below writes NULs into `line`, and a routed
       post crosses verbatim for the receiving engine's own entry to parse. */
    char *whole = strdup(line);

    CHECK(whole != NULL, "wpt: OOM keeping a host notice whole");
    /* No field of a notice holds a TAB (engine_notice_build asserts it), so the split is exact and a record
       longer than the longest this router reads is refused rather than glued. */
    for (q = line, f[nf++] = q; *q; q++)
        if (*q == '\t') {
            CHECK(nf < 1 + NAVIGABLE_CREATE_N, "wpt: a host notice has more fields than any record this router "
                                               "reads — the engine and this runner were built from different tables");
            *q = 0;
            f[nf++] = q + 1;
        }
    if (nf == 1 + NAVIGABLE_CREATE_N && !strcmp(f[0], "navigable.create")) {
        /* The fields are navigable_notice.h's create table. The provenance and the two marks are the child's
           arrival (solver/engine.h's engine_set_arrival); this runner decides no fetch from them, since it
           fetches only from wptserve with no cookie jar, and it keys nothing on the group. The token asserts
           check the record against the table this binary was compiled with. */
        DCHECK(!strcmp(WPT_CREATE(PROVENANCE), PENDING_PROVENANCE_OBSERVED) ||
               !strcmp(WPT_CREATE(PROVENANCE), PENDING_PROVENANCE_DERIVED) ||
               !strcmp(WPT_CREATE(PROVENANCE), PENDING_PROVENANCE_FORCED),
               "the create notice's provenance field is not one of the three provenance tokens engine.h declares — "
               "navigable.c composed it from navigable_notice.h's table, so the writer and this reader disagree");
        DCHECK(!strcmp(WPT_CREATE(GROUP), "creator") || !strcmp(WPT_CREATE(GROUP), "new"),
               "the create notice's group field is not `creator` or `new` — the record and the table have parted");
        DCHECK(!strcmp(WPT_CREATE(DRIVEN), PENDING_DRIVEN_YES) || !strcmp(WPT_CREATE(DRIVEN), PENDING_DRIVEN_NO),
               "the create notice's drive mark is not a token engine.h declares — the record and the table have "
               "parted");
        DCHECK(!strcmp(WPT_CREATE(PINNED), PENDING_PINNED_YES) || !strcmp(WPT_CREATE(PINNED), PENDING_PINNED_NO),
               "the create notice's witness mark is not a token engine.h declares — the record and the table "
               "have parted");
        char arrival[64];
        CHECK(snprintf(arrival, sizeof arrival, "%s,%s,%s", WPT_CREATE(PROVENANCE), WPT_CREATE(DRIVEN),
                       WPT_CREATE(PINNED)) < (int)sizeof arrival,
              "wpt: a create notice's three marks did not fit an arrival word — each is one of engine.h's words");
        char *policy = wpt_create_text(f, NAVIGABLE_CREATE_POLICY);
        char *policy_report_only = wpt_create_text(f, NAVIGABLE_CREATE_POLICY_REPORT_ONLY);

        wpt_spawn_child(WPT_CREATE(CHILD), WPT_CREATE(ADDRESS), WPT_CREATE(ORIGIN), policy, policy_report_only,
                        WPT_CREATE(CSP_SELF_ORIGIN), WPT_CREATE(TOP_LEVEL_URL), WPT_CREATE(COEP),
                        WPT_CREATE(COEP_ENDPOINT), WPT_CREATE(COEP_REPORT_ONLY),
                        WPT_CREATE(COEP_REPORT_ONLY_ENDPOINT), WPT_CREATE(REFERRER_POLICY), WPT_CREATE(PARENT),
                        WPT_CREATE(CONTAINER_POLICY), WPT_CREATE(ANCESTOR_ORIGINS), WPT_CREATE(SANDBOX_FLAGS),
                        WPT_CREATE(OPENER), arrival);
        free(policy);
        free(policy_report_only);
        free(whole);
        return;
    }
    /* `navigable.swap`, navigable_notice.h's provision record: HTML §7.1.3.2's browsing context group swap, the
       same provisioning act as a create in a different record. There is no creator (§7.3.2.3 creates the new
       browsing context "with null, null, and group"), so there is no policy container to clone and no opener
       policy to inherit; and the swapped-to navigable is a top-level traversable, so HTML §8.1.3.1's top-level
       creation URL is its own address, derived here and passed twice below. The provenance and both marks are
       the child's arrival (engine.h's engine_set_arrival) and decide no fetch. A new group is a new process here
       for the reason a cross-origin child is: SECURITY.md keys an instance on `(browsing context group, origin)`,
       and a swap changes the first half. */
    if (nf == 1 + NAVIGABLE_PROVISION_N && !strcmp(f[0], "navigable.swap")) {
        char arrival[64];
        CHECK(snprintf(arrival, sizeof arrival, "%s,%s,%s", WPT_PROVISION(PROVENANCE), WPT_PROVISION(DRIVEN),
                       WPT_PROVISION(PINNED)) < (int)sizeof arrival,
              "wpt: a provisioning record's three marks did not fit an arrival word");
        /* With no creator every container item takes its spec value. The CSP list is empty, and an empty policy
           resolves no `'self'`, so there is no self-origin. §7.1.4's item is "a new embedder policy", spelled out
           because §7.1.7 gives every container one: `unsafe-none` with two empty reporting endpoints. §7.3.1.3's
           parent is `u`: §7.1.3.2's swap is reached only for a top-level browsing context ("if browsingContext is
           not a top-level browsing context, then return browsingContext"). There is no container element, so
           Permissions Policy §9.7 step 1 is the answer, and §3.1.3's steps 2-3 return an empty list for a
           Document with no container document. */
        /* §7.1.5's set is empty, in that section's own word: a top-level browsing context's creation sandboxing
           flags are its popup sandboxing flag set, which §7.1.5 makes empty at creation and only §7.3.1.7's rules
           for choosing a navigable populate, and no `window.open` reaches a swapped-to context. */
        wpt_spawn_child(WPT_PROVISION(DOCUMENT), WPT_PROVISION(ADDRESS), WPT_PROVISION(ORIGIN), "", "", "",
                        WPT_PROVISION(ADDRESS), "unsafe-none", "", "unsafe-none", "",
                        FETCH_DEFAULT_REFERRER_POLICY, "u",
                        PERMISSIONS_POLICY_SERIALIZED_NO_CONTAINER,
                        DOCUMENT_ANCESTOR_ORIGINS_SERIALIZED_NONE,
                        SANDBOX_FLAGS_SERIALIZED_NONE, "N", arrival);
        free(whole);
        return;
    }
    /* `navigable.navigate`, the same provision record: a top-level traversable whose navigation response's origin
       is another agent's, with §7.1.3.2 keeping the browsing context (core/frame/navigable.c's
       nav_navigate_to_peer). The child is the swap's in every field: a top-level document with no creator-side
       container, no parent, no element presenting it and its own address as its top-level creation URL. The
       provenance and both marks are its arrival. */
    if (nf == 1 + NAVIGABLE_PROVISION_N && !strcmp(f[0], "navigable.navigate")) {
        char arrival[64];
        CHECK(snprintf(arrival, sizeof arrival, "%s,%s,%s", WPT_PROVISION(PROVENANCE), WPT_PROVISION(DRIVEN),
                       WPT_PROVISION(PINNED)) < (int)sizeof arrival,
              "wpt: a provisioning record's three marks did not fit an arrival word");
        wpt_spawn_child(WPT_PROVISION(DOCUMENT), WPT_PROVISION(ADDRESS), WPT_PROVISION(ORIGIN), "", "", "",
                        WPT_PROVISION(ADDRESS), "unsafe-none", "", "unsafe-none", "",
                        FETCH_DEFAULT_REFERRER_POLICY, "u",
                        PERMISSIONS_POLICY_SERIALIZED_NO_CONTAINER,
                        DOCUMENT_ANCESTOR_ORIGINS_SERIALIZED_NONE,
                        SANDBOX_FLAGS_SERIALIZED_NONE, "N", arrival);
        free(whole);
        return;
    }
    /* `windowproxy.post <target doc> <world> <marks> <target origin> <base64>`: §9.4.4 across instances. The
       origin is stamped here by the zone that knows who sent it; the engine may not name its own sender
       (SECURITY.md), so the notice carries none. */
    if (nf == 6 && !strcmp(f[0], "windowproxy.post")) {
        wpt_route_post(ctx, f[1], whole, from_origin);
        free(whole);
        return;
    }
    /* `world.gone <name>` and `world.parked <vector>`. They let a peer instance that never left memory release
       the segment it holds for a timeline of ours, and let the zone hold a foreign world's death for whichever
       instance later resumes this document. Here every peer is a child process this run reaps in the same
       teardown (wpt_children_free closes its pipes and waits for it), and no instance resumes a wpt file, so
       reaping is a stronger statement than either notice.
       Only `world.parked` is park-only: engine_notify_worlds_parked's one caller is inside the park (engine.c),
       so one outside a park is a park this host did not ask for. engine_notify_worlds_gone is also called by
       solver/flow.c's flow_release whenever a flow leaves the frontier, which is ordinary traffic. */
    /* `fetch.cancel <identity>`: no flow is owed this request any more. The identity is a park's
       `<method> <mode> <referrer policy> <referrer> <url>` (engine_pending_fetch_cancel) or a rendezvous id
       (engine_host_terminate), the key its reply would be delivered under. A send already on a connection
       finishes its bytes, since an HTTP/1.1 exchange ends only with its message or its connection, and is
       marked so its reply reaches nobody (wpt_request_finish releases a park's record). A park never sent has
       its record released here. */
    if ((nf == 6 || nf == 2) && !strcmp(f[0], "fetch.cancel")) {
        int i, sent = 0;
        uint32_t id = 0;

        if (nf == 2) {
            char *endp;
            unsigned long v = strtoul(f[1], &endp, 10);
            DCHECK(*f[1] && *endp == '\0' && v != 0 && v <= UINT32_MAX,
                   "a fetch.cancel notice's rendezvous id is not a number — engine_host_terminate writes the id "
                   "engine_host_requests listed");
            id = (uint32_t)v;
        }
        for (i = 0; i < g_inflight_n; i++) {
            WptRequest *r = &g_inflight[i];
            int named = nf == 2
                ? r->deliver != WPT_DELIVER_FETCH && r->deliver != WPT_DELIVER_SYNC && r->id == id
                : r->deliver == WPT_DELIVER_FETCH && !strcmp(r->method, f[1]) && !strcmp(r->mode, f[2]) &&
                  !strcmp(r->referrer_policy, f[3]) && !strcmp(r->referrer, f[4]) && !strcmp(r->url, f[5]);
            if (named && !r->cancelled) { r->cancelled = 1; sent = 1; }
        }
        for (i = 0; nf == 6 && !sent && i < g_owed_n; i++)
            if (!strcmp(g_owed[i].url, f[5]) && !strcmp(g_owed[i].method, f[1])) { wpt_owed_forget(i); break; }
        free(whole);
        return;
    }
    if (nf == 2 && (!strcmp(f[0], "world.gone") || !strcmp(f[0], "world.parked"))) {
        DCHECK(g_parking || !strcmp(f[0], "world.gone"),
               "a park's carried-segment announcement arrived while this run was still measuring — "
               "`world.parked` is written at ONE site (engine.c, inside the park) and names the foreign worlds "
               "that park is handing to the cold tier, so one outside a park is a park this host did not ask "
               "for and a residue nothing will resume. `world.gone` is NOT this case and is not asserted "
               "against: solver/flow.c's release announces one whenever a flow leaves the frontier, which is "
               "ordinary traffic");
        free(whole);
        return;
    }
    /* `document.seed <address> <provenance> <driven> <pinned> <issuer>`: an address the document under test declared is a
       page of its own application (solver/route_seed.h), reached whenever a test calls `history.pushState` with a
       URL, as `html/browsers/browsing-the-web/history-*` and many `navigation-api` tests do. This host refuses it:
       a WPT run is the conformance oracle for one document, whose verdict is its own subtests, so there is no
       frontier to rank an address on, and seeding would reach out to an address the test merely named. The
       refusal is explicit because a silently dropped notice is invisible at the emitting end (the DFAIL below).
       The grammar is still asserted: a malformed record is a writer that has changed, whatever this host does
       with it. */
    /* `navigable.request`, navigable_notice.h's request table: a child navigable's request facts
       (core/frame/navigable.c), which the trusted zone speaks a sub-frame seed with. A WPT run loads no seed, so
       the record is read for its grammar alone. */
    if (nf == 1 + NAVIGABLE_REQUEST_N && !strcmp(f[0], "navigable.request")) {
        DCHECK(*WPT_REQUEST(CREATOR) != '\0' && *WPT_REQUEST(DESTINATION) != '\0' &&
               *WPT_REQUEST(ADDRESS) != '\0',
               "a navigable.request notice named no creator, destination or address — core/frame/navigable.c "
               "writes all three non-empty");
        free(whole);
        return;
    }
    if (nf == 6 && !strcmp(f[0], ROUTE_SEED_NOTICE)) {
        DCHECK(*f[1] != '\0',
               "a route declaration named no address — solver/route_seed.c refuses an empty one at the "
               "emitting end, so a record that arrives with one is a writer this host no longer shares a "
               "grammar with and the field it read as an address is somebody else's");
        DCHECK(!strcmp(f[2], PENDING_PROVENANCE_DERIVED) || !strcmp(f[2], PENDING_PROVENANCE_FORCED),
               "a route declaration carries a provenance token that is neither `derived` nor `forced` — a "
               "declaration is made by RUNNING the page's code and no load of anything produces one, so "
               "`observed` is unreachable for it and any other word is a field read at the wrong offset");
        DCHECK(!strcmp(f[3], PENDING_DRIVEN_YES) || !strcmp(f[3], PENDING_DRIVEN_NO),
               "a route declaration carries a drive mark that is neither token engine.h declares");
        DCHECK(!strcmp(f[4], PENDING_PINNED_YES) || !strcmp(f[4], PENDING_PINNED_NO),
               "a route declaration carries a witness mark that is neither token engine.h declares");
        DCHECK(*f[5] != '\0', "a route declaration named no declaring document — solver/route_seed.c writes the "
                               "realm's world name");
        free(whole);
        return;
    }
    free(whole);
    DFAIL("a host notice reached the router under an operation it does not route — a notice this host drops is "
          "a capability the engine emitted for and nothing performs, which is invisible at the emitting end");
}

/* Route every notice this instance has emitted. Two callers at two moments: the slice boundary, and the end of
   the run, where the park emits between writing the residue and closing the session (the engine's last point at
   which a notice of ours is still drained), and the loop in main leaves on the DONE that same step answers. */
static void wpt_drain_notices(JSContext *ctx)
{
    const char *p;

    /* The origin stamped below is this process's, and only the parent process has one. SECURITY.md puts the
       origin stamp in the zone, so a notice leaves carrying the origin of the document that emitted it, which
       wpt_derive_addresses establishes and only the top-level runner runs. A `--document` child takes its origin
       from its command line, so routing a notice from one would stamp the empty string, which every
       `event.origin` check in the corpus reads as a real origin. */
    DCHECK(g_top_origin[0] != 0,
           "a host notice is being stamped with an origin this process never established — wpt_derive_addresses "
           "sets it and a `--document` child does not run it, so this notice would leave under the empty origin");

    /* A document announced but not yet provisioned is one a read would arrive for before its instance exists,
       which is why these are drained ahead of the requests. */
    for (p = engine_host_notices(); *p; ) {
        const char *end = strchr(p, '\n');
        char *line;
        size_t n;

        if (!end) break;
        n = (size_t)(end - p);
        /* The buffer is sized to the record: a routed message is base64 and routinely long, and a dropped notice
           is a delivery the peer never makes. */
        line = malloc(n + 1);
        CHECK(line != NULL, "wpt: OOM reading a host notice");
        memcpy(line, p, n); line[n] = 0;
        wpt_route_notice(ctx, line, g_top_origin);
        free(line);
        p = end + 1;
    }
}

/* Route or answer every host request. Returns whether it answered anything, because the step loop needs to
   know: a stall this host answers nothing to is a frontier waiting on a reply that is never coming, rather than
   one waiting on this host (see the loop in main). A flow blocked on an unanswered request reports itself
   host-owed and leaves the pick, so the scheduler, not this file, prevents a spin. */
static bool wpt_answer_host_requests(JSContext *ctx)
{
    const char *p;
    bool answered = false;

    wpt_drain_notices(ctx);

    /* Each request is routed to the instance holding its document, as the trusted zone does in the extension:
       only it knows which instance holds which document. */
    for (p = engine_host_requests(); *p; ) {
        /* `id<TAB>asker<TAB>op`: `tab` is the TAB before the operation. The asker is the zone's to judge a
           request by; this runner serves the corpus off loopback and judges nothing. */
        const char *idtab = strchr(p, '\t');
        const char *tab = idtab ? strchr(idtab + 1, '\t') : NULL;
        const char *end = strchr(p, '\n');
        char op[512];
        uint32_t id;
        size_t n;

        if (!tab || !end) break;
        DCHECK(tab > idtab + 1 && tab < end, "an owed host request is not `id<TAB>asker<TAB>op` — "
                                             "engine_host_requests writes the asking document on every record");
        id = (uint32_t)strtoul(p, NULL, 10);
        n = (size_t)(end - tab - 1);
        /* Every cross-agent operation routes the same way: a member of a navigable's Window, the close of a
           traversable, and the internal methods a lent object performs ([[Get]], [[Set]], [[Delete]],
           [[Call]]). They differ in what the peer resolves, not in who resolves it, so this is a prefix and
           not a list: a row added to core/frame/remote_op.c's OPS table reaches its instance with nothing here
           to remember. */
        if (!strncmp(tab + 1, "windowproxy.", 12) || !strncmp(tab + 1, "object.", 7)) {
            /* The record is relayed whole, so it is copied at its own length: an operation can carry a value (an
               encoded string is base64, an argument list has no length). */
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
                /* An operation for a document nothing provisioned is a missing instance, not a slow answer: the
                   notice that named it was dropped or the operation outran it. Left unanswered, the flow would
                   park for ever and the file would time out with nothing said. */
                DCHECK(c != NULL, "a cross-agent operation named a document this host never provisioned — the "
                                  "navigable.create notice for it was dropped, or this is a CHILD instance "
                                  "asking its PARENT, the reverse of this transport's one direction: a child "
                                  "writes notices unasked in its boot round, but never asks for an operation");
                if (c) {
                    /* The zone's rendezvous token travels with the operation, because the performing instance
                       answers by emitting a completion that names one (engine.h's engine_perform). This channel
                       carries one question at a time, so the asking request's own id cannot collide on it, and the
                       zone mints it because a request id is unique only inside the instance that minted it. */
                    size_t opcap = strlen(rec) + 32;
                    char *ask = malloc(opcap);
                    const char *answer;
                    CHECK(ask != NULL, "wpt: OOM asking a child document to perform an operation");
                    snprintf(ask, opcap, "op\t%lu\t%s", (unsigned long)id, rec);
                    answer = wpt_child_ask(c, ask);
                    free(ask);
                    if (answer) {
                        /* The answer's grammar is remote_object.c's in both processes and both directions: an
                           object name says which agent's namespace it is in, so a name that came home resolves to
                           the original object. An answer is a completion, since the peer ran a program and may
                           have thrown; this zone relays the type and the asking flow raises the throw at the call
                           site that parked. The answer first names the child's timeline, separated by the one TAB
                           this line splits on: a world vector's separators are ':' and ',' (world_serialize), so
                           the completion, which may contain a tab, is the remainder. */
                        int completion = ENGINE_COMPLETION_NORMAL;
                        const char *sep = strchr(answer, '\t');
                        const char *msep = sep ? strchr(sep + 1, '\t') : NULL;
                        JSValue v;
                        char *world, *marks;
                        CHECK(sep != NULL && sep != answer,
                              "wpt: a child answered a cross-agent operation without naming the timeline that "
                              "computed it — a document's state is its flows, so an unnamed answer cannot be "
                              "told from another of that document's timelines answering the same question");
                        CHECK(msep != NULL && msep != sep + 1,
                              "wpt: a child answered a cross-agent operation without its timeline's marks — "
                              "perform_answer_notify writes them beside the world on every answer");
                        world = strndup(answer, (size_t)(sep - answer));
                        marks = strndup(sep + 1, (size_t)(msep - sep - 1));
                        CHECK(world != NULL && marks != NULL,
                              "wpt: OOM reading the timeline that answered a cross-agent operation");
                        v = remote_completion_decode(ctx, msep + 1, &completion);
                        engine_host_answer(ctx, id, world, marks, v, completion, ENGINE_ANSWER_PEER);
                        /* The child asserts a second completion cannot exist on this channel, so this one is the
                           last and the asking family's standby has nothing left to wait for. */
                        engine_host_exhausted(ctx, id);
                        free(world);
                        free(marks);
                        answered = true;
                        JS_FreeValue(ctx, v);
                    }
                }
            }
            free(rec);
        }
        /* HTML §7.4.5 Populating a session history entry's create navigation params by fetching, answered here
           because the document being loaded belongs to the instance that asked; routing it to a peer would answer
           a same-origin load out of another agent. A null body is a fetch that did not load, which is still a
           document the navigable gets. */
        else if (!strncmp(tab + 1, "document.fetch\t", 15)) {
            FetchRequest req = {0};   /* §2.2.5 metadata stated as "nothing yet": these are the runner's own
                                     HTTP client requests and reach no park, so a zero is the named abort a
                                     future reader gets rather than a stack address it would compare bytes at */
            HeaderList none = { 0 };

            DCHECK(n < sizeof op, "a document address outgrew this runner's request buffer — truncating it "
                                  "would GET a different URL and skipping it parks the loading flow forever, "
                                  "so the buffer is what has to grow");
            memcpy(op, tab + 1, n); op[n] = 0;
            /* Asked, not awaited, and here waiting would deadlock: the document §7.4.5 fetches may need an
               instance this process has yet to fork, and a host standing in the read could not spawn it, route
               to it or run the flow that would. The answer is built at completion (wpt_request_finish's DOCUMENT
               arm), where the response's header list and bytes arrive together. */
            if (!wpt_request_asked_id(id)) {
                /* `document.fetch<TAB><provenance><TAB><driven><TAB><pinned><TAB><destination><TAB><referrer
                   policy><TAB><initiator><TAB><url>`: the address is the remainder past the seventh tab. The provenance, marks and
                   referrer policy are read nowhere here, for the router's reason (a corpus fetch at wptserve
                   spends no session, so no firing decision and no Referer), but must be skipped. The destination
                   is the request's. */
                char *prov_at = strchr(op, '\t');
                char *drv_at = prov_at ? strchr(prov_at + 1, '\t') : NULL;
                char *pin_at = drv_at ? strchr(drv_at + 1, '\t') : NULL;
                char *dest_at = pin_at ? strchr(pin_at + 1, '\t') : NULL;
                char *rp_at = dest_at ? strchr(dest_at + 1, '\t') : NULL;
                char *init_at = rp_at ? strchr(rp_at + 1, '\t') : NULL;
                char *url_at = init_at ? strchr(init_at + 1, '\t') : NULL;

                DCHECK(url_at != NULL && url_at[1],
                       "a document.fetch request carried no ADDRESS after its provenance, two marks, destination, "
                       "referrer policy and initiator — the load job writes `document.fetch<TAB><provenance><TAB><driven>"
                       "<TAB><pinned><TAB><destination><TAB><referrer policy><TAB><initiator><TAB><url>` "
                       "(core/frame/navigable.c), "
                       "so a missing tab means this host and that job no longer share a grammar");
                req.method = "GET";
                req.url = url_at + 1;
                /* §7.4.5's navigation request's destination, as the job stated it: `document`, or the
                   navigable's container's local name. Terminated in this host's own copy of the record. */
                *rp_at = '\0';
                req.destination = dest_at + 1;
                /* A navigation's mode is `navigate`: Fetch §2.2.5 "Requests" enumerates it and HTML §7.4.5's
                   create navigation params by fetching creates its request with that mode. */
                req.mode = FETCH_MODE_NAVIGATE;
                req.headers = &none;
                req.body = NULL;
                req.body_len = 0;
                wpt_request_begin(&req, WPT_DELIVER_DOCUMENT, id);
                answered = true;
            }
        }
        /* XHR §3.5.6's fetch, answered with the same HTTP as every other request. The record crosses as JSON
           because it carries a method, a header list and a body, which a tab-separated line cannot hold. It is
           answered here, not routed: an XMLHttpRequest belongs to the instance that made it, and the zone's job
           for one is the network. */
        else if (!strncmp(tab + 1, "xhr.send\t", 9)) {
            char *line;
            JSValue rec, mv, uv, hv, bv;
            const char *method, *url, *bodys = NULL;
            size_t blen = 0;
            HeaderList hdrs = { 0 };
            FetchRequest req = {0};   /* §2.2.5 metadata stated as "nothing yet": these are the runner's own
                                     HTTP client requests and reach no park, so a zero is the named abort a
                                     future reader gets rather than a stack address it would compare bytes at */
            uint32_t hn = 0, hi;

            /* The register lists this until it is answered; see wpt_issue_pending's guard. */
            if (wpt_request_asked_id(id)) { p = end + 1; continue; }
            line = malloc(n + 1);
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
            /* XMLHttpRequest's destination is Fetch §2.2.5's empty string, its own row of that section's
               initiator/destination table, the `connect-src` one it shares with `fetch()`. A value, not a hole:
               these bytes are data. */
            req.destination = "";
            /* XHR §3.5.6 "The send() method" initializes its request with `mode` `cors`, the value
               core/xhr/xml_http_request.c states, because this is the same request. */
            req.mode = FETCH_MODE_CORS;
            req.headers = &hdrs;
            req.body = bodys;
            req.body_len = blen;
            /* Asked, not awaited. `xml_http_request.c` calls this park "the one rendezvous in this engine that
               BLOCKS a flow": the flow blocks, the thread it shares with every other flow of the document does
               not. The whole request is serialized into the record before this returns, so nothing borrowed here
               outlives the call. */
            wpt_request_begin(&req, WPT_DELIVER_XHR, id);
            answered = true;
            header_list_free(&hdrs);
            if (method) JS_FreeCString(ctx, method);
            if (url) JS_FreeCString(ctx, url);
            if (bodys) JS_FreeCString(ctx, bodys);
            JS_FreeValue(ctx, mv); JS_FreeValue(ctx, uv); JS_FreeValue(ctx, hv); JS_FreeValue(ctx, bv);
            JS_FreeValue(ctx, rec);
        }
        /* An operation this host does not route is a flow parked on an answer nobody can send, so it crashes
           here, naming the verb, instead of a file that reports nothing until the wall backstop. A missing route
           is a capability this host has not built. Every op the engine raises is routed above (windowproxy.*,
           object.*, document.fetch, xhr.send), so this fires only for the next one added. */
        else {
            DFAIL("a cross-agent operation reached this host under a verb it does not route — the flow that "
                  "asked is parked on an answer nothing in this process will ever produce. Route it beside "
                  "the four above, or state why this host is not the zone that answers it");
        }
        p = end + 1;
    }
    return answered;
}

/* ---- The test file, which is often a document ----------------------------------------------------------
 *
 * Most of the corpus is markup documents, so collecting only `.any.js` and `.window.js` would exclude them. A
 * `.window.js` is a script WPT's server wraps in a minimal document; an `.html` file is the document. The only
 * difference is which bytes this runner parses: after that, both run their scripts against the same Window.
 *
 * The document is fetched, not read off disk: wptserve substitutes a `.sub.html` (hosts, ports, the local
 * origin), so the bytes on disk are a template. The driver says which kind a test is (`--test-document`): it
 * collected the file because it is a markup document that loads testharness.js, and the corpus spells one
 * `.html`, `.htm` and `.xhtml`, so a second rule here would disagree with engine/wpt.mjs's collection rule. */
static int    g_html_mode;      /* the test is a document, not a script */
static char   g_test_url[512];  /* its address, server-relative: what to GET and what to resolve against */
/* The variant is part of the address. WPT's manifest emits one test per `<meta name=variant>` / `// META:
   variant=` declaration, at the file's own URL with that query or fragment appended (tools/manifest/sourcefile.py:
   `test_url + variant`), and the file reads it back off `location.search` to choose its subtests. Running the bare
   URL runs a test the corpus does not run, so the driver names the variant and it lands in the two addresses
   everything else derives from: what to GET, and what `location` answers. */
static char   g_variant[256];   /* "" or the "?…"/"#…" this run is */

/* A page error as it happens: an uncaught throw from one of the programs the scheduler runs (solver/result.h).
   It is a report, never a verdict: a script that throws does not stop the document, so the flow runs on to the
   next program as it would in Chrome, and testharness's subtest results decide the file. The throw site is its
   own column, distinct from the test's address, because a WPT document loads scripts from `/resources/` and its
   own directory. §8.1.4.6 "Runtime script errors"' `filename` is "" for a thrown value with no backtrace, printed
   as `-`. A retraction is a second line (see solver/result.h): the `@WPTERR` is already in the stream when
   §8.1.6.4 step 7.4 says the page handled the rejection after all, so the correction appends under its own token,
   which a reader matches to the first line by the pair. */
static void wpt_page_error(const char *msg, const char *filename, ResultPageErrorEdge edge)
{
    /* A switch, not a ternary, so an edge added to that enum is a compiler error here rather than a line printed
       under the wrong token. */
    const char *doc = g_test_url[0] ? g_test_url : "<document>";
    const char *at = filename && *filename ? filename : "-";
    switch (edge) {
    case RESULT_PAGE_ERROR_STANDS:
        fprintf(report_out(), "@WPTERR %s at=%s: %s\n", doc, at, msg); break;
    case RESULT_PAGE_ERROR_RETRACTED:
        fprintf(report_out(), "@WPTERR-RETRACTED %s at=%s: %s\n", doc, at, msg); break;
    /* This host prints no exploration classification at STANDS, so a reversal of one has nothing to correct. */
    case RESULT_PAGE_ERROR_OWNED: break;
    }
}

/* This document's program sequence, what a session is opened over (engine.h). Each entry is its own program body
   and its own JS_FlowNew: classic scripts do not share a top-level scope, and concatenating them would leak
   per-<script> let/const bindings between files. The scheduler borrows the arrays for the life of the session,
   so they are freed with the run. `body` is taken, `name` is copied, and `type` is HTML §4.12.1.1's script type,
   which selects §8.1.4.4's algorithm. A document test takes it from its <script> element; every program this
   runner supplies (prologue, report, epilogue, the driver's META scripts, a `.any.js` test) is classic.
   `name` is what this host calls a program (wpt_insert_report finds the harness by it). `address` is HTML §8.1.4.1
   "Scripts"' base URL, "the URL from which the script was obtained, for external scripts, or the document base
   URL of the containing document, for inline scripts", which the engine hands to §8.1.4.4 as the script's base
   and a module's map key. A synthesized program has no address, and a row is source text or an address
   (solver/engine.c's seed asserts it). */
static char **g_prog_bodies, **g_prog_names, **g_prog_srcs;
static ScriptType *g_prog_types;
static int    g_prog_n, g_prog_cap;

static void wpt_program(char *body, const char *name, const char *address, ScriptType type)
{
    /* An address is a URL or absent. This runner's own programs are named `<wpt-…>`; that spelling in the address
       column would be resolved as a `src`, used as the script's base URL and, for a module, as its map key. */
    DCHECK(address == NULL || address[0] != '<',
           "a program synthesized by this runner was given an ADDRESS — no bytes came from one, so the name "
           "this host calls it by has been passed as the base URL HTML §8.1.4.1 Scripts says a script is "
           "obtained from");
    if (g_prog_n == g_prog_cap) {
        int cap = g_prog_cap ? g_prog_cap * 2 : 8;
        char **b = realloc(g_prog_bodies, (size_t)cap * sizeof *b);
        char **nm = realloc(g_prog_names, (size_t)cap * sizeof *nm);
        char **u = realloc(g_prog_srcs, (size_t)cap * sizeof *u);
        ScriptType *t = realloc(g_prog_types, (size_t)cap * sizeof *t);
        CHECK(b != NULL && nm != NULL && u != NULL && t != NULL,
              "wpt: OOM building this document's program sequence — a dropped program "
              "is a document that runs something other than what it was served");
        g_prog_bodies = b; g_prog_names = nm; g_prog_srcs = u; g_prog_types = t; g_prog_cap = cap;
    }
    g_prog_bodies[g_prog_n] = body;
    g_prog_names[g_prog_n] = strdup(name);
    g_prog_srcs[g_prog_n] = address ? strdup(address) : NULL;
    g_prog_types[g_prog_n] = type;
    CHECK(body != NULL && g_prog_names[g_prog_n] != NULL && (!address || g_prog_srcs[g_prog_n] != NULL),
          "wpt: OOM naming a program");
    g_prog_n++;
}

/* The report's position, asked of the program sequence rather than of the mode: immediately after
   `/resources/testharness.js`, the slot WPT's testharnessreport.js occupies. That is one rule for a document
   (whose <script src> the corpus wrote) and a `.any.js` (whose harness the driver passes as argv[1]). A sequence
   with no harness is reported, not crashed: it is a document whose `<script src>` 404'd (already printed, and
   the driver's ABORT) or a file that is not a testharness test, neither a missing capability. Running it without
   the hook would make it look like a file with no subtests. */
static void wpt_insert_report(void)
{
    static const char TAIL[] = "/resources/testharness.js";
    size_t m = sizeof TAIL - 1;
    int at = -1, k;

    /* Asked of the name column, which every program has. The address column is NULL for programs no address
       supplied, which includes the driver's META scripts and the `.any.js` harness this position is defined by. */
    for (k = 0; k < g_prog_n; k++) {
        size_t n = strlen(g_prog_names[k]);
        if (n >= m && !strcmp(g_prog_names[k] + n - m, TAIL)) { at = k + 1; break; }
    }
    if (at < 0) {
        fprintf(report_out(), "@WPTERR %s: no %s among this document's programs, so the runner's report hook "
                              "could not be installed and nothing would report this file's subtests\n",
                g_test_url, TAIL);
        return;
    }
    wpt_program(strdup(WPT_REPORT), "<wpt-report>", /*address*/NULL, SCRIPT_TYPE_CLASSIC);
    {   /* appended, then rotated into place — one slot, four parallel arrays */
        char *body = g_prog_bodies[g_prog_n - 1], *nm = g_prog_names[g_prog_n - 1];
        char *src = g_prog_srcs[g_prog_n - 1];
        ScriptType t = g_prog_types[g_prog_n - 1];
        size_t move = (size_t)(g_prog_n - 1 - at);
        memmove(g_prog_bodies + at + 1, g_prog_bodies + at, move * sizeof *g_prog_bodies);
        memmove(g_prog_names + at + 1, g_prog_names + at, move * sizeof *g_prog_names);
        memmove(g_prog_srcs + at + 1, g_prog_srcs + at, move * sizeof *g_prog_srcs);
        memmove(g_prog_types + at + 1, g_prog_types + at, move * sizeof *g_prog_types);
        g_prog_bodies[at] = body; g_prog_names[at] = nm; g_prog_srcs[at] = src; g_prog_types[at] = t;
    }
}


/* One GET against the corpus server, resolved against the running document's address. `pheaders`, when given,
   receives the response's whole header list, the caller's to header_list_free: §7.5.1 creates a Document from a
   header list, and a corpus test's `.headers` sidecar or `?pipe=header(...)` can declare
   `Origin-Agent-Cluster`, `Cross-Origin-Opener-Policy` or `Cross-Origin-Embedder-Policy`. */
static char *wpt_get_headers(const char *url, size_t *plen, HeaderList *pheaders)
{
    FetchRequest req;
    HeaderList h;
    int status = 0;
    char *body;

    memset(&req, 0, sizeof req);
    memset(&h, 0, sizeof h);
    req.method = "GET";
    /* This runner's own fetch of a test resource, a request no page made: `no-cors` is Fetch §2.2.5's "unless
       stated otherwise" and the answer for a fetch no algorithm created. */
    req.mode = FETCH_MODE_NO_CORS;
    req.url = url;
    body = wpt_http(&req, plen, &status, &h);
    if (pheaders) *pheaders = h;   /* moved, not copied; the caller frees it */
    else header_list_free(&h);
    if (body && (status < 200 || status >= 300)) { free(body); body = NULL; }
    return body;
}

static char *wpt_get(const char *url, size_t *plen)
{
    return wpt_get_headers(url, plen, NULL);
}

/* The run's own file did not arrive: a fact about the corpus, not an invariant of this engine.
 *
 * A CHECK would be wrong here: check.h reserves always-fatal asserts for OOM, physical floors, security
 * boundaries and data integrity, and an absent input is none of those. It is reported like this file's other
 * absent inputs (a `<script src>` the server lacks, an unreadable META script, an argv that is not the corpus's
 * testharness.js). Three states must not share one answer: the engine named a capability it lacks, the corpus
 * could not present the test, and a resource backstop fired. engine/wpt.mjs reads the middle one off a marker,
 * never off prose, and counts it as `corpus`.
 *
 * One function because it is one question asked at two entries: a document run fetches the test file inside
 * wpt_build_document and a script run fetches it beside its programs. */
static void wpt_unserved_test_file(void)
{
    fprintf(report_out(), "@WPTERR %s: the corpus server did not serve this run's own file\n", g_test_url);
    /* Nothing was measured, so there is nothing to tear down and no result to print; a program whose input is
       absent has no work to do, as main's `usage:` arm says. `exit` flushes stdio, which puts the line above in
       front of the driver. The status is 0 because a signal or a non-zero code is how the driver recognises its
       own interventions. */
    exit(0);
}

static void wpt_derive_addresses(int argc, char **argv)
{
    const char *h = argv[1], *tail = strstr(h, "/resources/testharness.js");
    const char *test = argv[argc - 1];
    size_t n = tail ? (size_t)(tail - h) : 0;

    /* The corpus root is derived from the harness path, so a first argument that is not the corpus's
       testharness.js would leave it empty, and every address below would carry a filesystem path into a URL. */
    CHECK(tail != NULL, "wpt: argv[1] is not the corpus's resources/testharness.js, so the corpus root is unknown");
    CHECK(n < sizeof g_wpt_root, "wpt: the corpus root is longer than this runner holds");
    memcpy(g_wpt_root, h, n);
    g_wpt_root[n] = 0;
    /* The last argument is the test; the ones between are its META scripts. A document test and a script test
       each live at their own corpus path, so both have one address: what to GET, and what a relative URL resolves
       against. Both are fetched from the server, so a `.sub.` script test runs substituted. */
    CHECK(!strncmp(test, g_wpt_root, n), "wpt: a test outside the corpus root has no server address");
    {
        /* Where the corpus is served from. The driver starts wptserve and names its loopback port; the runner
           sends the URL's own authority as the Host header, as a hosts-file mapping does for a real browser. A run
           with no server is a gate defect rather than a result, so it fails loud. */
        const char *sv = getenv("WPT_SERVER");
        CHECK(sv && *sv && strchr(sv, ':'),
              "wpt: WPT_SERVER names no host:port — the driver must start engine/wptserve.py and pass it");
        CHECK(strlen(sv) < sizeof g_server, "wpt: WPT_SERVER is longer than this runner holds");
        snprintf(g_server, sizeof g_server, "%s", sv);
    }
    {
        /* What it is served as: the authority the corpus's documents are written against, composed by
           engine/wptserve.py from the config wptserve's `sub` pipe substitutes `{{host}}` and
           `{{ports[http][0]}}` from. It is not composed here because the port is ephemeral, bound per run. There
           is no default: an unestablished origin would resolve every test's URLs against an authority nothing
           served them from. */
        const char *og = getenv("WPT_TOP_ORIGIN");
        const char *colon = strchr(g_server, ':');
        int server_port = colon ? atoi(colon + 1) : -1;
        UrlRecord o;
        int parsed;

        CHECK(og && *og, "wpt: WPT_TOP_ORIGIN names no origin — the driver must read the authority off "
                         "engine/wptserve.py's READY line and pass it, and this runner has no literal to use "
                         "instead: the port in it is bound per run");
        CHECK(strlen(og) < sizeof g_top_origin, "wpt: WPT_TOP_ORIGIN is longer than this runner holds");
        snprintf(g_top_origin, sizeof g_top_origin, "%s", og);
        /* And it agrees with the socket the documents come off. The two facts arrive on two environment variables
           and describe one server: wptserve binds `ports[http][0]` and substitutes that number into every `.sub`
           document. It is parsed, not scanned: WHATWG URL §4.4 "URL parsing" decides where the port begins, and
           `port` is -1 for the spec's null port, which a port-less origin gives and this assert catches. */
        url_record_init(&o);
        parsed = url_parse(&o, g_top_origin, strlen(g_top_origin), NULL);
        DCHECK(parsed, "wpt: WPT_TOP_ORIGIN is not a URL, so the authority every test in this run resolves "
                       "against is a string the URL parser rejects");
        DCHECKF(o.port == server_port,
                "wpt: the corpus is served AS `%s` and fetched FROM `%s` — those name two different ports, so "
                "every test that asserts a resolved URL would compare an authority against bytes that came "
                "from another one. The driver reads both off the same READY line; one of them is stale",
                g_top_origin, g_server);
        url_record_free(&o);
    }
    snprintf(g_test_url, sizeof g_test_url, "%s%s%s", g_top_origin, test + n, g_variant);
    snprintf(g_base_url, sizeof g_base_url, "%s", g_test_url);
}

static lxb_html_document_t *g_wpt_dom;   /* the runner's parsed document */

/* What a document is, built by one function for every kind. A test document and a child document (the navigable
 * an <iframe> or a popup created) differ in three things: the name the world registry knows them by, their
 * origin, and which bytes their tree is parsed from. The platform surface, browsing context, prototype chain
 * and event loop are the same machinery.
 *
 * `html`/`html_n` is the document to parse; NULL means the minimal wrapper WPT puts around a .window.js.
 * `g_base_url` and `g_server` must already name where this document lives (see wpt_derive_addresses). */
static JSRuntime *g_rt;

/* The agent and the document are split because they have different lifetimes. An agent is a JSRuntime: the
 * class registry every component registers into, the world registry that names documents, the flow frontier. A
 * document is a JSContext in it: a global with the platform installed, a parsed tree, an address. A same-origin
 * child navigable is a second document in this agent, so a document's description runs more than once.
 *
 * What each half is lives in core/platform.h's one list, shared by every host. This function keeps only what is
 * this runner's own: the solver bootstrap it needs to exercise real components, and its network edge. */
static void wpt_agent_init(JSContext *ctx, const char *doc_name, const char *origin,
                           const char *top_level_url, bool requests_oac, OpenerPolicyValue opener_policy)
{
    /* The components under test are not named here. core/platform.h holds the one list, so this host cannot
       omit a component and publish a number for an area it never ran; it can only add. */

    /* Name this document. A WindowProxy answers "is this navigable remote?" against the document identity the
       world registry owns, so the registry is up before the first proxy exists. */
    world_registry_init(doc_name);
    /* The concolic value class. This runner exercises the real components, and several answer with a concolic
       value where the spec's answer is unknown input (`window.name` survives navigation, so an attacker who can
       open the document sets it). The class is registered before any component can mint one, and location.c's
       two attacker sources are declared out of it. */
    concolic_init(ctx);
    /* The value semantics, without the absent-global source. A conformance run reaches concolic values (the two
       Location sources are the document's own address), and every operator over one needs the hooks. A global
       that was never set must not become unknown input: the spec answers it with a ReferenceError, and the corpus
       tests that. */
    concolic_install_hooks();
    /* This host declares that it wants the spec's own values, so "this host does not explore" is a stated answer
       rather than the same state as "no host has decided yet"; core/realm.c refuses a realm built before the
       question is answered. It must precede platform_agent_init, which ends by running every per-realm
       intrinsic. */
    concolic_declare_browser_only();
    /* The solver frontier, because a member that suspends parks on a flow's pending register and that register
       is the frontier's. Its contents are the scheduler's: engine_sched_begin seeds it and asserts it finds it
       empty, so an agent init that put a flow there could not be the agent of a session. */
    flow_registry_init(doc_name);
    /* The scope the endpoint surface qualifies its identities by: a request a test makes is recorded like any
       other, and solver/novelty.h refuses an identity asked before an instance names its origin. */
    novelty_set_scope(origin);
    /* The DOM chokepoint's context. §4.2.3's insertion and removing steps are fired from the solver's tree
       chokepoint, which needs the runtime they run in; without it no <script> is prepared, no custom element
       upgrades and no §4.8.5 child navigable is created. */
    dom_cow_set_ctx(ctx);
    /* This host's network edge: who answers, the one thing that is this runner's rather than the platform's. An
       edge is a parameter, and fetch.c aborts on a fetch issued with none. What it does with the flow is the
       product's park (wpt_owe): the request goes on that flow's own pending register. The page error hook names
       who reports a page error while the programs are the scheduler's. */
    { static const FetchProvider P = { wpt_owe, engine_pending_fetch_cancel }; fetch_set_provider(&P); }
    result_set_page_error_hook(wpt_page_error);
    /* No string-handler edge is stated: HTML §8.7's timer initialization steps compile a DOMString handler as a
       classic script when the timer fires (substeps 9.8.7-9.8.8, inside step 9's task), and core/timing/timer.c
       does that in the task's own step machine, so there is nothing for a host to register or omit. */

    /* Then the facts, stated rather than assigned. `requests_oac` is §7.5.1's, from the response the caller
       fetched: a WPT test document is fetched from wptserve, so it has a response, and
       `html/browsers/origin/origin-keyed-agent-clusters/` delivers this header only through `?pipe=header(...)`
       and `.headers` sidecars. `opener_policy` is §7.1.3's, which §7.3.2.3's group is created with. This host reads
       neither header itself: the list goes through §7.4.2.1 "Supporting concepts"' navigation params, created from
       the response by §7.4.5 "Populating a session history entry", as the product host's does. */
    platform_agent_init(ctx, origin, top_level_url, requests_oac, opener_policy);
}

/* One document. Runs once per document including the first, so it is the one description of a document of this
   build; a same-origin child navigable gets exactly this. */
static void wpt_realm_install(JSContext *ctx, lxb_html_document_t *dom, const char *url, const char *origin,
                              DocumentKind kind,
                              SerializedPolicyContainer policy,
                              SerializedResponsePermissionsPolicy permissions_policy,
                              SandboxFlags sandbox_flags,
                              uint32_t doc_id, JSValueConst nav_proxy, const char *about_base_url,
                              bool is_iframe_srcdoc)
{
    JSValue global = JS_GetGlobalObject(ctx);

    /* `self` is not set here. It is §7.2.2's [Replaceable] Window member and window_install installs it as one; a
       plain own value written first would stand in for the member under test and hide its absence. */
    JS_SetPropertyStr(ctx, global, "print", JS_NewCFunction(ctx, js_wpt_print, "print", 1));
    /* `gc`, what /common/gc.js reaches for first, and the only way a test that asserts a stream survives
       collection can assert anything. It is the runner's, never the browser's: a page-visible collector is
       fingerprintable and this engine does not ship one. quickjs's JS_RunGC is the whole of it. */
    JS_SetPropertyStr(ctx, global, "gc", JS_NewCFunction(ctx, js_wpt_gc, "gc", 0));
    /* `wptTestComplete`, the vendor hook's other half, called from the completion callback WPT_REPORT registers.
       It is this runner's own, like the two above. */
    JS_SetPropertyStr(ctx, global, "wptTestComplete",
                      JS_NewCFunction(ctx, js_wpt_test_complete, "wptTestComplete", 0));

    /* Then the platform: every component, in core/platform.h's one order. The lines above are added to it and
       nothing here subtracts from it. `document` comes last in that list because of §4.8.5: installing it runs the
       insertion steps for every <iframe> in the markup, which creates a child navigable and so needs the
       WindowProxy class and §7.4's create a navigable first. A Window is installed at the address, not the
       origin; the two are separate parameters. */
    platform_document_install(ctx, global, dom, url, origin, kind, policy, permissions_policy, sandbox_flags,
                              doc_id, nav_proxy, about_base_url, is_iframe_srcdoc);

    JS_FreeValue(ctx, global);
}

/* A same-origin child navigable's realm: a second JSContext in the same JSRuntime, which is HTML's similar-origin
   window agent. It gets the identical per-document install the first document got; a child with a smaller
   `window` would be a different browser. */
static JSContext *wpt_child_realm(JSRuntime *rt, lxb_html_document_t *dom, const char *url,
                                  const char *top_level_url, const char *origin, DocumentKind kind,
                                  SerializedPolicyContainer policy,
                                  SerializedResponsePermissionsPolicy permissions_policy,
                                  SandboxFlags sandbox_flags,
                                  uint32_t doc_id, JSValueConst nav_proxy, const char *about_base_url,
                                  bool is_iframe_srcdoc)
{
    JSContext *ctx = JS_NewContext(rt);

    CHECK(ctx != NULL, "wpt: a same-origin child navigable's realm could not be created");
    /* §3.7: a realm gets its own intrinsics. Members run in the realm that defined them, so a child sharing the
       agent realm's EventTarget.prototype would resolve every unqualified `addEventListener` against the parent's
       window. They come from the one list components declare themselves into. §7.4 decided the child's top-level
       creation URL; using `url` here would make an about:blank iframe of an http page a secure context. */
    /* §3.3.7 step 1: a child navigable is a Window; §8.1.3.5 step 1.2.1: a Window has no owner set. */
    realm_install_intrinsics(ctx, platform_window_agent(), top_level_url, "Window", false);
    wpt_realm_install(ctx, dom, url, origin, kind, policy, permissions_policy, sandbox_flags, doc_id,
                      nav_proxy, about_base_url, is_iframe_srcdoc);
    /* The child's scripts run in its realm, and they are not run here. A realm is built from inside a navigation's load
       job, a C activation in the middle of some flow's program, so running them now would re-enter JS from C.
       navigable_seed_scripts (core/frame/navigable.c) queues them, for every host, as programs of the one
       frontier on the flow that created the navigable, naming this document: that flow's timeline is the one this
       child exists in, and the queue entry naming the document compiles them in the realm just built. */
    return ctx;
}

/* The first document, which also brings the agent up. A real Lexbor parse, the one the engine runs, of the test's
   own bytes when the test is an HTML file and otherwise of the minimal document WPT's server wraps a `.window.js`
   in. `html_headers` is the response `html` is the body of, and the two travel together or not at all, as
   core/frame/navigable.c's child_document asserts: a Document is created from a response. NULL for both is HTML
   §7.4's initial about:blank, an HTML document by §7.4 rather than by a default here. */
/* The inherited arguments are HTML §7.1.7 "Policy containers"' clone of the creator's container, for a
   `--document` child provisioned from another process's `navigable.create`; the top-level document has no
   creator and passes empty values. `inherited_csp` and `inherited_csp_self_origin` are its CSP list. The four
   `inherited_coep*` are its §7.1.4 embedder policy item, as §7.1.4's tokens: clone a policy container sets
   "clone's embedder policy to a copy of policyContainer's embedder policy", so a clone without it would put a
   cross-origin-isolated creator's child under `unsafe-none`. The remaining four are not container items (the
   container is five policies) and are computed by the creating process, which holds their inputs:
   `parent_navigable` is HTML §7.3.1.3 "Child navigables"' parent in remote_object.h's identity form (`u` for the
   top-level document, which nothing embeds); `container_policy` is the container element's Permissions Policy
   §9.5 result; `ancestor_origins` is HTML §3.1.3 "Ancestor origins"' internal ancestor origin objects list,
   which a serialization cannot recompute because every opaque origin serializes to `null`; and
   `creation_sandbox_flags` is HTML §7.1.5 "Sandboxing"'s set, which §7.4.2.2 unions with the response's
   CSP-derived flags. The top-level document passes each grammar's word for "none". */
static JSContext *wpt_build_document(const char *doc_name, const char *origin, const char *top_level_url,
                                     const char *html, size_t html_n, const HeaderList *html_headers,
                                     const char *inherited_csp, const char *inherited_csp_report_only,
                                     const char *inherited_csp_self_origin,
                                     const char *inherited_coep, const char *inherited_coep_endpoint,
                                     const char *inherited_coep_report_only,
                                     const char *inherited_coep_report_only_endpoint,
                                     const char *inherited_referrer_policy,
                                     const char *parent_navigable, const char *container_policy,
                                     const char *ancestor_origins, const char *creation_sandbox_flags,
                                     const char *opener_navigable)
{
    /* §7.1.5's set, parsed before anything is built from it; see the read below. */
    SandboxFlags creation_flags = 0;
    static const char DOC[] = "<!doctype html><html><head></head><body></body></html>";
    char *fetched = NULL;
    HeaderList response_headers;
    /* The response this document is created from: the caller's when it fetched one, this function's own when it
       fetched the test file itself, and NULL when there is none. One name, because §7.4.5 reads a response
       whichever zone obtained it. */
    const HeaderList *response = NULL;
    /* §7.4.5's computed type for the document about to be built, and whether there is one. With no response (§7.4's
       initial about:blank, or an address whose fetch failed) there is no type to compute and the document is HTML
       by §7.4, so the parse runs §7.4 rather than §7.4.5's dispatch. It lives across the whole function because
       the type and the bytes travel together to the loader. */
    MimeType computed;
    bool computed_defined = false;
    /* §7.5.1's type and content type for the document this runner loads, decided by the §7.4.5 dispatch the parse
       runs and carried to the install rather than restated there. */
    DocumentKind root_kind;
    /* HTML §13.2.3.2 "Determining the character encoding"'s answer for this response, and the characters its
       decode produced. `decoded` replaces `src` at the parse when there is a response, and lives across the whole
       function because core/loader/document_load.h borrows the characters for the life of the load. -1 is the
       no-response arm: §7.4's initial about:blank keeps DOM §4.5 "Interface Document"'s utf-8 default. */
    int root_encoding = -1;
    char *decoded = NULL;
    size_t decoded_n = 0;
    NavigationParams np;
    const char *src = html;
    JSRuntime *rt;
    JSContext *ctx;

    DCHECK((html != NULL) == (html_headers != NULL),
           "the runner was asked to build a Document out of half a response — MIME Sniffing §7's computed type "
           "is a fact about the bytes those headers describe, so bytes with no header list is a document whose "
           "type was never asked and a header list with no bytes is one computed for another response");

    /* The response is read before the agent exists, in the spec's order: §7.5.1 reads `Origin-Agent-Cluster` off
       the response and hands the answer to §8.1.2.2's obtain a similar-origin window agent, which allocates the
       agent, so the cluster key must be known first (the standard calls the environment the read happens in
       reserved). The bytes come with the headers: wptserve's `.headers` sidecars and `?pipe=header(...)` are the
       whole delivery mechanism for areas like `html/browsers/origin/origin-keyed-agent-clusters/`. */
    memset(&response_headers, 0, sizeof response_headers);
    if (src) {
        response = html_headers;
    } else {
        src = DOC;
        html_n = sizeof DOC - 1;
        if (g_html_mode) {
            fetched = wpt_get_headers(g_test_url, &html_n, &response_headers);
            if (!fetched) wpt_unserved_test_file();
            src = fetched;
            response = &response_headers;
        }
    }
    /* §7.4.2.1 "Supporting concepts"' target snapshot params' sandboxing flags are the caller's answer (the
       struct's own member, not §7.4.2.3.4 "Non-fetch schemes and external software"'s like-named row). For the
       top-level document it is §7.1.5's empty set: no embedder element, so §7.1.5 answers from the popup
       sandboxing flag set, which only §7.3.1.7 "Navigable target names"' rules for choosing a navigable fill. A
       `--document` child is rooted in a navigable another process created, whose set is its embedder's
       `<iframe sandbox>` attribute unioned with the embedder document's own, so both callers state it. The other
       half of §7.4.2.2's union is this response's CSP-derived flags, which navigation_params computes. It is
       parsed, then asserted, never parsed inside the DCHECK, whose condition is compiled out in release.
       §8.1.3.5's secure-context answer is over the environment's top-level creation URL, which decides whether an
       `Origin-Agent-Cluster`, a COOP or a COEP is honoured at all. */
    {
        bool ok = sandbox_flags_of_serialized(creation_sandbox_flags, &creation_flags);

        (void)ok;
        DCHECK(ok,
               "a document was built with no readable §7.1.5 CREATION SANDBOXING FLAG SET — the process that "
               "created this navigable writes that section's own flag names comma-separated on the command "
               "line, and its word for the empty set where there are none, so a field that is empty or "
               "carries a word outside those names is a caller that stopped stating it rather than a "
               "navigable with no flags. Read as the empty set it would run a sandboxed frame's document with "
               "its sandbox deleted");
    }
    /* The params come from the response this document is created from, whichever zone obtained it; a
       `--document` child's caller fetched its bytes and headers. A document with no response gets an empty list,
       which is right for it: §7.4's initial about:blank has no policy of its own and inherits its creator's,
       which is the clone applied below. */
    navigation_params_from_response(&np, response ? response : &response_headers, creation_flags,
                                    secure_context_url_potentially_trustworthy(top_level_url));
    /* HTML §7.4.5's first step, "Let type be the computed type of navigationParams's response", asked here where
       the response is and carried to the parse with the bytes it describes; the same component answers it for
       core/frame/navigable.c's child_document. wptserve serves `.xhtml`/`.xht`/`.xml` as `application/xhtml+xml`
       or `application/xml`, MIME Sniffing §7 step 1 keeps that supplied type, and §7.4.5 sends it to §7.5.3. Under
       the HTML parser a `<script>` is raw text (HTML §13.2.6.4.4 The "in head" insertion mode switches to
       §13.2.5.4 Script data state), so XML §2.7 "CDATA Sections"' `<![CDATA[` and XML §4.6 "Predefined
       Entities"' `&lt;` would reach the JavaScript compiler as program text.
       HTML §13.2.3.2 "Determining the character encoding" runs beside it over the same response and raw bytes, so
       a file served `windows-1252`, or whose `<meta charset>` says so, decodes correctly and reports it in
       `document.characterSet`. The type is computed from the raw bytes (MIME Sniffing §5.2 "Reading the resource
       header") and the parse runs on the decode (HTML §13.2.3.1 "Parsing with a known character encoding"). There
       is no parent encoding: §13.2.3.2's container step has no container for the runner's top-level document. */
    if (response) {
        document_load_computed_type(&computed, response, src, html_n);
        computed_defined = true;
        root_encoding = document_load_decode(&decoded, &decoded_n, response, src, html_n,
                                             /*parent_encoding*/-1);
    }
    /* The record outlives the header list it was computed from (mime_sniff_computed's answer owns its strings),
       so the list is released at its last read. The decode above had to come first: §13.2.3.2 reads the
       `Content-Type` header. */
    header_list_free(&response_headers);

    rt = JS_NewRuntime();
    JS_SetMaxStackSize(rt, 4 * 1024 * 1024);
    ctx = JS_NewContext(rt);
    wpt_agent_init(ctx, doc_name, origin, top_level_url, np.requests_oac, np.opener.value);
    /* §7.4 calls back here for a same-origin child, because what a document of this build is is this runner's
       answer: a child with a different platform surface would make every number measured in it a number about a
       different browser. */
    navigable_set_realm_builder(wpt_child_realm);

    g_wpt_dom = dom_document_create();
    CHECK(g_wpt_dom != NULL, "the runner's document allocation failed");
    /* Shared, for main.c's reason: this is the runner's active document and every flow reads its tree. The two
       calls are §7.4.5's load a document and §7.4's initial about:blank, the split child_document makes. The
       parser for a response is whichever one §7.4.5's arm names, and the assert keeping the HTML parser from a
       type it does not serve is at that loader (core/loader/html_document.c). The response arm parses the decode
       and the no-response arm parses the bytes it was handed, this build's constant or a WPT wrapper. §13.2.4.5
       is enabled: this runner executes the test document's scripts, so its parser takes a browser's arm. */
    CHECK((computed_defined
             ? document_load(g_wpt_dom, DOM_PARSE_ROOT_SHARED, HTML_SCRIPTING_ENABLED, &computed, root_encoding,
                             (const lxb_char_t *)decoded, decoded_n)
             : html_parse_document(g_wpt_dom, DOM_PARSE_ROOT_SHARED, HTML_SCRIPTING_ENABLED,
                                   (const lxb_char_t *)src, html_n)) == LXB_STATUS_OK,
          "the runner's document did not parse");
    /* §7.5.1's type and content type for this document, off the computed type the parse dispatched on, read before
       the record is freed and never restated at the install. The no-response arm is §7.3.2.1 "Creating browsing
       contexts"' constant, the split the two parse calls make. */
    root_kind = computed_defined ? document_load_kind(&computed) : document_kind_initial_about_blank();
    if (computed_defined)
        mime_type_free(&computed);
    free(fetched);
    /* The decode is borrowed for the life of the load (core/loader/document_load.h), and the load above has
       completed, so the bytes are this frame's to release. */
    free(decoded);

    /* The root navigable is the host's, so its §7.2.3 proxy is minted here, as a child's creator mints its
       proxy: whoever owns a navigable makes its one proxy. */
    {
        /* The runner loaded this document itself, so the navigable's name is the spec's initial "" and
           `window.name` stays a computed value. §7.5.1's Document comes from the navigation params the response
           decided: §7.4.5's final sandboxing flag set and §7.1.7 step 3's CSP list come out of the one place a
           response is read (core/frame/navigation_params.c), as for the product host.
           The CSP list is HTML §7.1.7's determine navigation params policy container, called rather than
           restated. §2.2.2 "Parse response's Content Security Policies" gives a response-delivered list
           "response's URL's origin", which for a document this runner fetched is `origin`; §7.1.7's clone keeps the
           origin it was cloned from, which for a `--document` child is its creator's, from the command line. The
           self-origin is separate from `origin` because they are two facts (a Document's principal, and the origin
           `'self'` resolves against), and an inherited container makes them disagree. */
        EmbedderPolicyValue _coep = EMBEDDER_POLICY_UNSAFE_NONE, _coep_ro = EMBEDDER_POLICY_UNSAFE_NONE;
        SerializedPolicyContainer inherited;
        /* §7.1.4's three strings, read back (see embedder_policy.h for why they cross as tokens). A token naming
           none of them came off this runner's own command line, written by its parent, not off a header
           (§7.1.4.1's fail-open is `obtain`'s, run in the parent), so it crashes rather than defaulting. */
        SerializedPolicyContainer response;
        SerializedPolicyContainer policy;
        /* The parse is a statement and the assert only reads it: a DCHECK condition is compiled out in release. */
        bool _ok = embedder_policy_value_of_token(inherited_coep, &_coep);
        bool _ok_ro = embedder_policy_value_of_token(inherited_coep_report_only, &_coep_ro);

        (void)_ok; (void)_ok_ro;
        DCHECK(_ok && _ok_ro,
               "a document was built with an §7.1.4 EMBEDDER POLICY VALUE naming none of the three strings the "
               "section defines — this runner writes these tokens onto a child's command line itself, so a "
               "token outside them is this program's two ends disagreeing about their own record");
        inherited = serialized_policy_container_or_none(
            inherited_csp, inherited_csp_report_only, inherited_csp_self_origin,
            serialized_embedder_policy(_coep, inherited_coep_endpoint, _coep_ro,
                                       inherited_coep_report_only_endpoint),
            /* Named residual. Not covered: §7.1.7's integrity policy item on this runner's command-line relay of
               a child process's inherited container (the engine's two wires, the navigate job's arguments and the
               parked navigable record, carry it). Next diff builds: a flag written at the parent and read here.
               Absence shows as: an integrity-policy subtest that passes in-instance failing across the process
               boundary. */
            NULL, inherited_referrer_policy);
        /* §7.5.1's opener policy row for this root Document (§7.1.3's policy, from the same response as the
           params) and §7.3.1.3's parent, which for a `--document` child is a navigable in the parent process and
           for the top-level test document is none. Both go to core/frame/navigable.c's navigable_root, where
           §7.1.4.2's check over the two containers also runs, so this runner and the product host root a
           navigable through one algorithm. After the inherited container, because that check reads it. */
        JSValue root_proxy = navigable_root(ctx, world_local_doc(), "", np.opener.value, parent_navigable,
                                            inherited, &np.embedder, creation_flags);

        CHECK(!JS_IsException(root_proxy), "the root navigable's WindowProxy could not be allocated");
        window_proxy_set_cross_origin_framing(root_proxy, navigation_params_allows_cross_origin_parent(&np));
        /* §7.3.1.3's container, before the install below: Permissions Policy §9.5 runs once per Document a
           navigable is given, and for a navigable rooted in another process's frame tree only that process could
           run it. Stated after the rooting, in §7.3.1.3's order, as the product host states it. */
        navigable_root_container(ctx, root_proxy, container_policy);
        /* §7.2.2.4's opener, which an auxiliary navigable has in the process that opened it. */
        navigable_root_opener(ctx, root_proxy, opener_navigable);
        /* §3.1.3's ancestor origins for that navigable's Document, before the install for the same reason:
           §7.3.2.1 runs those steps once per Document a navigable is given and reads this there. A third
           statement, not a field of either above (see navigable.h). */
        navigable_root_ancestor_origins(ctx, root_proxy, ancestor_origins);
        /* §7.1.4's item of the response's own container: §7.1.7's create a policy container from a fetch
           response step 4, obtained where the response is read and handed to the constructor here. */
        response = serialized_policy_container(np.csp, np.csp_report_only, origin,
                                               serialized_embedder_policy_of(&np.embedder),
                                               np.integrity_policy, np.referrer_policy);
        policy = policy_container_determine_navigation_params(g_base_url, response, inherited);

        wpt_realm_install(ctx, g_wpt_dom, g_base_url, origin, root_kind, policy,
                          serialized_response_permissions_policy(np.permissions_policy,
                                                                 np.permissions_policy_report_only),
                          np.sandbox_flags, world_local_doc(), root_proxy,
                          /* §7.4's about base URL: NULL. The root document is created from a response, the
                             arm §2.4.3 "Document base URLs" answers with the document's own address; a child
                             navigable's arrives on the builder above. */
                          /*about_base_url*/ NULL,
                          /* HTML §4.8.5's class: false. A root document is not the product of an
                             `<iframe srcdoc>` navigation; a child navigable's answer arrives on the builder
                             above. */
                          /*is_iframe_srcdoc*/ false);
        /* HTML §13.2.3.2 "Determining the character encoding"'s answer, written onto the Document here, as
           core/frame/navigable.c and main.c write it: the Document record is the install's product, so this is
           the first moment there is one. -1 is the no-response arm, which keeps DOM §4.5 "Interface Document"'s
           default. */
        if (root_encoding >= 0)
            document_set_encoding(ctx, root_encoding);
        /* …and HTML §7.7's input from the same response. */
        document_state_x_frame_options(ctx, np.x_frame_options, np.n_x_frame_options);
        JS_FreeValue(ctx, root_proxy);
    }
    navigation_params_free(&np);   /* document_install built its container from it and keeps no pointer */
    /* Sealed after the first document only: a component declares its IDL members in its init and installs from
       the cached id, so a second realm's install declares nothing, and a declaration reached from it is a
       component that mints per global. */
    idl_args_seal();
    g_rt = rt;
    return ctx;
}

/* The child document's own main. It is a document, not a test: it parses its address's bytes, runs that
 * document's scripts, and then answers questions; it prints no results, because it has none.
 *
 * It answers by running a program, not by reading a property from C. `self.length` is an IDL getter and the
 * engine refuses to reach one from a C activation with no flow base under it, so a read is a flow like every
 * other, which also lets an answer suspend. The asking world arrives with its ancestry: engine_route and
 * engine_perform materialize its segment where they attach the record (forking the nearest ancestor this instance
 * holds), flow_deliver and flow_perform ask again when the work runs, and flow_switch_in/out switch timelines.
 * Two arms of one fork writing through one reference are two timelines, and a peer that performed both against
 * one baseline would build a third. What an operation is (verb, fields, operands, performing program) is
 * core/frame/remote_op.h's, shared with the production ABI entry. What is left here is this host's channel (one
 * line in, one line out) and the routing of what arrives on it. */
/* A child's notices ride back on the answer pipe prefixed `N\t`, unasked in its boot round and ahead of an
   answer after it, and the parent's router takes them. They are drained after every step, because a held notice is a delivery its target has not been
   told about. `remoteop.answer` is not a notice but the completion of the operation this channel asked for, so
   it leaves as the answer line; it is recognised by its rendezvous token. A document with N timelines answers N
   times, and a second answer is something this channel cannot yet carry, not something to overwrite the first.
   `g_child_answer` is `<world><TAB><marks><TAB><completion>` for the token in flight, owned here until written;
   the world lets the parent tell a second timeline's answer from one answer relayed twice. */
static char *g_child_answer;

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
                /* The remainder, `<world><TAB><marks><TAB><completion>`, crosses whole: the world is the flow of this instance
                   that computed the answer, and the parent needs it to tell the peer's other timelines from one
                   answer delivered twice. Its presence is asserted at the parent's decode, where the split is. */
                DCHECK(memchr(tab + 1, '\t', n - (size_t)(tab + 1 - p)) != NULL,
                       "a cross-agent operation's answer carried no TIMELINE before its completion — every "
                       "answer names the world of the flow that ran the program (flow_answer_perform), so this "
                       "notice was written by something that does not share that grammar and the parent would "
                       "read the world's bytes as the head of the completion record");
                g_child_answer = strndup(tab + 1, n - (size_t)(tab + 1 - p));
                CHECK(g_child_answer != NULL, "wpt: OOM keeping a cross-agent operation's completion");
                p = end + 1;
                continue;
            }
        }
        /* The in-flight token's exhaustion is this channel's own fact: it carries one answer per question, so the
           parent retires the asker's standby with that answer (wpt's engine_host_exhausted call). */
        if (token && n == 19 + strlen(token) && !strncmp(p, "remoteop.exhausted\t", 19) &&
            !memcmp(p + 19, token, strlen(token))) {
            p = end + 1;
            continue;
        }
        fputs("N\t", stdout);
        fwrite(p, 1, n, stdout);
        fputc('\n', stdout);
        p = end + 1;
    }
}

/* This host's loop over the one scheduler, a host with nothing else to do between quanta (engine.h). Every
   program this instance runs (its document's scripts, a delivery's task, the program that performs a peer's
   operation) is a work item on the one frontier, preemptible and parkable at any depth; this only hands the
   thread over and takes it back. It ends at the stall, never at DONE: a document a peer holds a reference into
   may not run out of timelines (engine.h's engine_set_referenced), so its frontier stalls waiting for the next
   thing on this channel, which is where the thread belongs. */
static void wpt_child_run(JSContext *ctx, const char *token)
{
    for (;;) {
        int r = engine_sched_step();
        int did;
        wpt_child_emit_notices(token);
        if (r == ENGINE_STEP_YIELD) continue;
        /* What this host owes, at the same boundary as the top-level half pays it: a child document fetches too,
           and a reply nobody supplies is a timeline parked for the life of the instance. A stall this fills is
           not a stall. */
        did = wpt_issue_pending();
        if (wpt_net_pump(ctx, /*block*/0) > 0) did = 1;
        if (did) continue;
        /* Nothing runnable and nothing filled. With an answer on its way this instance waits in `poll`, so the
           wait ends at whichever request lands first. With nothing in flight this is the stall the parent's next
           question resumes. */
        if (g_inflight_n) { wpt_net_pump(ctx, /*block*/1); continue; }
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
    /* The arguments are appended in the order they were added, so every position already read keeps its meaning.
       Only the CSP pair defaults, because its empty value is the spec's absence; for the rest the checks below
       refuse a short command line, since "not stated" and a legitimate "none" would otherwise be one value.
       HTML §8.1.3.1's top-level creation URL, from the parent that created this navigable. This document may be
       nested, and answering from its own address would make an https child of an http page a secure context,
       the ancestral hole Secure Contexts §4.2 closes. */
    const char *top_level_url = argc > 6 ? argv[6] : NULL;
    /* CSP §2.2's self-origin of the inherited list in `csp`: §2.2.2 states a self-origin from outside the policy
       bytes ("response's URL's origin"), and this process's own address is the wrong one for a clone of the
       creator's container (CSP §2.2's note names documents "that have inherited their policy"). An empty pair
       spells CSP §2.2's absence. */
    const char *csp_self_origin = argc > 7 ? argv[7] : "";
    /* §7.1.4's embedder policy of that inherited container, its four items. §7.1.7's clone moves a container
       whole, and the item belongs to the creator's response, so nothing in this child's response could supply it.
       An embedder policy has no absence (§7.1.7 gives every container one), so "not stated" and "stated
       `unsafe-none`" must stay distinct: that is the difference between a child a browser isolates and one it
       does not. */
    const char *coep = argc > 8 ? argv[8] : NULL;
    const char *coep_endpoint = argc > 9 ? argv[9] : NULL;
    const char *coep_report_only = argc > 10 ? argv[10] : NULL;
    const char *coep_report_only_endpoint = argc > 11 ? argv[11] : NULL;
    /* HTML §7.3.1.3 "Child navigables"' parent of the navigable this process is rooted in, which a child process
       cannot see. `u`, the encoding's undefined, is a real answer (§7.1.3.2's swap sends it), the difference
       between a frame and a page. */
    const char *parent_navigable = argc > 12 ? argv[12] : NULL;
    /* §7.3.1.3's container of that navigable: Permissions Policy §9.5's result for the element presenting it, in
       core/permissions_policy/permissions_policy.h's serialization, or that grammar's "there is no container".
       §9.7 step 1's "if container is null, return `Enabled`" is a real answer for a swap, so taking it by default
       would hand a cross-origin frame every supported feature its embedder holds. */
    const char *container_policy = argc > 13 ? argv[13] : NULL;
    /* HTML §3.1.3 "Ancestor origins"' internal ancestor origin objects list for the Document this process is about
       to build, composed by the parent from a tree only it holds. The empty list is a real answer for a swap (a
       top-level traversable takes §3.1.3 step 3), so taking it by default would make a cross-origin frame report
       itself the top of its tree to every `location.ancestorOrigins` read. */
    const char *ancestor_origins = argc > 14 ? argv[14] : NULL;
    /* HTML §7.1.5 "Sandboxing"'s creation sandboxing flag set, computed by the parent from the `<iframe sandbox>`
       element and its node document's active set. The empty set is a real answer for a swap, so taking it by
       default would run a sandboxed frame's scripts, submit its forms and relax its `document.domain`. */
    const char *creation_sandbox_flags = argc > 15 ? argv[15] : NULL;
    /* HTML §7.2.2.4's opener of this navigable, in the parent's grammar (`N` for none): an auxiliary navigable's
       opener is a browsing context in the parent process. */
    const char *opener_navigable = argc > 16 ? argv[16] : NULL;
    /* How the parent reached this document, as engine.h's arrival word (the create or swap notice's provenance,
       drive mark and witness mark); engine_arrival_of_token refuses an absent or malformed one. */
    const char *arrival = argc > 17 ? argv[17] : NULL;
    /* §7.1.7's referrer policy item of the inherited container, appended last. Not defaulted: "" is a value. */
    const char *referrer_policy = argc > 18 ? argv[18] : NULL;
    /* The report policies of that container's CSP list (CSP §2.2), appended last; "" is a list with none. */
    const char *csp_report_only = argc > 19 ? argv[19] : NULL;
    JSContext *ctx;
    char *html = NULL;
    size_t html_n = 0;
    /* The response's header list, which travels with its bytes: HTML §7.4.5's computed type and §7.5.1's
       `Origin-Agent-Cluster`, opener policy, response CSP and CSP-derived sandboxing flags are read off it, and a
       `?pipe=header(...)` is how `html/browsers` delivers them to a child. */
    HeaderList html_headers;
    char line[65536];   /* a routed message rides this channel base64'd; see the post arm below */
    /* The session's program sequence, which outlives every statement in this function: the scheduler borrows
       these arrays for the life of the session (engine_sched_begin). */
    DocScripts ds;

    /* The answer pipe is stdout here, so nothing else may write to it; see report_out. */
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
    CHECK(parent_navigable && *parent_navigable,
          "wpt: a child document was started with no §7.3.1.3 PARENT NAVIGABLE — the section makes \"is a child "
          "navigable\" mean \"its parent is non-null\", and this process cannot see what embeds it, so without "
          "the field its document would be rooted in a navigable that presents as a TOP-LEVEL TRAVERSABLE: "
          "§7.1.4.2 step 1 returns true for it, `parent` and `top` answer itself, and §7.5.9's subtree walk "
          "finds nothing above it — four plausible answers about a page that is not there");
    CHECK(coep && coep_endpoint && coep_report_only && coep_report_only_endpoint,
          "wpt: a child document was started with no §7.1.4 EMBEDDER POLICY for its §7.1.7 policy container — "
          "the clone that created this Document moves every item of its creator's container, and this one is "
          "not derivable here: it belongs to the CREATOR's response, which this process never saw. Reading it "
          "as `unsafe-none` would put a cross-origin-isolated creator's child under the wrong rule for every "
          "no-CORS fetch it makes, with nothing anywhere to say so");
    CHECK(container_policy && *container_policy,
          "wpt: a child document was started with no §7.3.1.3 CONTAINER statement — Permissions Policy §9.5 is "
          "\"given null or an element (container) and an origin (origin)\", and BOTH of those arguments belong "
          "to the process that created this navigable: it holds the element and it computed this document's "
          "origin. So §9.5 runs there and its ANSWER arrives here. Without the field this process would take "
          "§9.7 step 1 for a navigable that HAS a container, returning `Enabled` for every supported feature "
          "the embedder was never asked about");
    CHECK(ancestor_origins && *ancestor_origins,
          "wpt: a child document was started with no §3.1.3 ANCESTOR ORIGINS statement — the steps read the "
          "parent Document's own recorded list, that Document's ORIGIN RECORD and the container element, and "
          "all three belong to the process that created this navigable. So §3.1.3 runs there and its ANSWER "
          "arrives here. Without the field this process would install the EMPTY list, which is the positive "
          "claim that this Document has no ancestors — a cross-origin frame telling every "
          "`location.ancestorOrigins` read that it is the top of its own tree");
    CHECK(opener_navigable && *opener_navigable,
          "wpt: a child document was started with no §7.2.2.4 opener navigable — an identity and `N` are the "
          "two answers, so an absent field is a parent that did not state which applies");
    CHECK(creation_sandbox_flags && *creation_sandbox_flags,
          "wpt: a child document was started with no §7.1.5 CREATION SANDBOXING FLAG SET — the section reads "
          "the embedder ELEMENT's iframe sandboxing flag set and that element's node document's active set, "
          "and both belong to the process that created this navigable. So §7.1.5 runs there and its ANSWER "
          "arrives here. Without the field this process would take the EMPTY set, which is not an absence but "
          "the positive claim that nothing about this frame is sandboxed — it would run the scripts, submit "
          "the forms and relax the `document.domain` its embedder's markup forbids");
    CHECK(referrer_policy && fetch_is_referrer_policy(referrer_policy),
          "wpt: a child document was started with no Referrer Policy §3 value for its §7.1.7 container's "
          "referrer policy — the parent writes the creator's item, or the initial default where there is no "
          "creator, so a child without it would send referrers its creator's policy strips");
    CHECK(csp_report_only != NULL,
          "wpt: a child document was started with no report-only half of its §7.1.7 container's CSP list — the "
          "parent writes the creator's report policies or \"\", so a child without it would fire none of the "
          "reports its creator's policy asks for");
    snprintf(g_base_url, sizeof g_base_url, "%s", url && *url ? url : "about:blank");
    /* `about:blank` has no bytes to fetch: its Document is the empty one §7.4 creates, which makes it synchronous
       in a browser and an <iframe> with no src scriptable immediately. */
    memset(&html_headers, 0, sizeof html_headers);
    if (strncmp(g_base_url, "about:", 6) != 0) {
        html = wpt_get_headers(g_base_url, &html_n, &html_headers);
        /* A child whose address does not load still has a document (a browser shows an error page and the
           navigable exists), so it is about:blank rather than a dead instance. A fetch that did not load is not a
           response, so its half-filled list is released and the pair that reaches the build is (NULL, NULL). */
        if (!html) { html_n = 0; header_list_free(&html_headers); memset(&html_headers, 0, sizeof html_headers); }
    }
    /* The top-level creation URL came from the parent with the rest of the environment, because this document may
       be nested in a document of another instance and only the creating zone knows which. */
    ctx = wpt_build_document(name, origin, top_level_url, html, html_n, html ? &html_headers : NULL,
                             csp, csp_report_only, csp_self_origin, coep, coep_endpoint, coep_report_only,
                             coep_report_only_endpoint, referrer_policy, parent_navigable, container_policy,
                             ancestor_origins, creation_sandbox_flags, opener_navigable);
    free(html);
    header_list_free(&html_headers);

    /* This instance exists because a peer created its navigable and still holds a WindowProxy for it, so its
       timelines may not run out (engine.h's engine_set_referenced). It is set before the session: otherwise the
       one flow finishes with its last script, the session closes, and a peer's first question arrives at a
       document with no timeline to answer in. The host states it because provisioning is the host's fact. */
    engine_set_referenced(1);
    engine_set_arrival(engine_arrival_of_token(arrival));

    /* The two intrinsics a peer performs a write and a call through are captured by the component that performs
       them, at this realm's install, before this document's scripts run, so a page cannot replace them on
       `Reflect`. There is nothing for this host to do. */

    /* This document's scripts are the session's program sequence, handed to the one scheduler. An external script
       stays an external row, `(bodies[i] NULL, srcs[i] its address)`, as core/loader/document_scripts.h declares:
       the flow parks on it, wpt_issue_pending asks for it with the rest of the pending list (method, destination,
       initiator, provenance, credentials mode and URL), and wpt_request_finish delivers a reply carrying the
       status. So the document's scripts go through §8.1.4.2 "Fetching scripts" as the shipped host's do. A
       `<script src>` the server answers 404 is then §4.12.1.1 "Processing model"'s step 4 ("If el's result is
       null, then fire an event named error at el, and return"), never an empty program that reaches step 8's
       `load`. */
    ds = document_exec_scripts(g_wpt_dom);
    engine_sched_begin(ctx, ds.bodies, ds.srcs, ds.types, ds.sched, ds.els, ds.n, /*forking*/0, NULL);
    /* The document runs before the first question arrives, which makes a child a participant rather than an empty
       frame: message-opener.html's whole body is one script that posts to its opener, and that post must be on
       this channel before the parent asks anything. */
    wpt_child_run(ctx, NULL);
    /* The end of this child's boot round: everything it wrote so far it wrote unasked, and from here it writes
       only to answer (the parent's pump stops watching this channel on reading it). */
    fputs("S\n", stdout);
    fflush(stdout);

    while (fgets(line, sizeof line, stdin)) {
        char *nl = strchr(line, '\n');
        /* A record that did not fit arrives as two, and the tail would be read as an operation of its own. The
           channel is line-oriented, so the only safe answer to a truncated read is to stop. */
        CHECK(nl != NULL || feof(stdin), "wpt: a cross-instance record was longer than this child's line buffer");
        char *answer;
        /* The channel's two shapes. Only the first field is this host's; the rest is the emitting engine's record,
           verbatim, for the entry that reads it. The zone adds the two things a record may not carry itself: the
           sender's origin, which only the trusted zone may state (SECURITY.md), and the rendezvous token, because a
           request id is unique only inside the instance that minted it.
             `post <sender origin> <record>`  : §9.4.4 step 7 arriving. Nothing is asked; a delivery is made.
             `op <token> <record>`            : a cross-agent operation this document is asked to perform. */
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
            /* Nothing runs inside this call. The record becomes a work item of every live timeline of this document,
               each delivering under its own delta when the scheduler runs it: the page's `message` listener was
               registered by a script, so it lives in the delta of the flow that ran that script. */
            engine_route(ctx, rec, f1);
            wpt_child_run(ctx, NULL);
            /* The ack is the transport's, not a program's completion: nothing was asked. It goes through the one
               encoder so this pipe has one grammar. */
            answer = remote_completion_encode(ctx, ENGINE_COMPLETION_NORMAL, JS_UNDEFINED);
        } else {
            /* A peer answers by running a program, a flow on this instance's frontier, so an answer can suspend and
               every other timeline keeps running meanwhile. This host hands the operation over and steps the
               scheduler until nothing is runnable; the completion arrives as a notice naming the token, which
               wpt_child_emit_notices takes. A throw is the answer and needs nothing here: flow_answer_perform
               encodes the completion with its type, so a peer's `try`/`catch` runs at the line that asked. */
            engine_perform(ctx, f1, rec);
            wpt_child_run(ctx, f1);
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
    wpt_net_free();
    return 0;
}

int main(int argc, char **argv)
{
    /* Line-buffered, so an abort cannot discard what the file already reported: a DCHECK ends the process with
       abort(), which does not flush stdio, and stdout is a pipe here, so it would otherwise be fully buffered and
       the driver would read a file that reported results as one that produced nothing. */
    setvbuf(stdout, NULL, _IOLBF, 0);
    JSRuntime *rt;
    JSContext *ctx;
    int i, failed = 0;

    g_self_exe = argv[0];
    /* A child navigable's instance, re-executed from this same binary; see the child-document section. */
    if (argc >= 5 && !strcmp(argv[1], "--document")) return wpt_child_main(argc, argv);
    /* The driver's classification, consumed before anything else reads argv, so everything downstream sees one
       argument shape and one indexing rule. */
    while (argc >= 2 && argv[1][0] == '-' && argv[1][1] == '-') {
        if (!strcmp(argv[1], "--test-document")) { g_html_mode = 1; argv++; argc--; }
        else if (!strcmp(argv[1], "--variant")) {
            CHECK(argc >= 3, "wpt: --variant names no variant");
            /* The corpus's own rule, asserted where the value arrives: sourcefile.py rejects a non-empty variant
               that does not begin with `?` or `#`, because a variant is a query or a fragment. Anything else would
               become part of the path, and this runner would GET a file that does not exist. */
            CHECK(argv[2][0] == '?' || argv[2][0] == '#',
                  "wpt: a variant must begin with ? or # — it is a query or a fragment, not a path");
            CHECK(strlen(argv[2]) < sizeof g_variant, "wpt: the variant is longer than this runner holds");
            snprintf(g_variant, sizeof g_variant, "%s", argv[2]);
            argv += 2; argc -= 2;
        } else break;
    }
    /* The harness and the test are both required: with only the harness given, every rule below that takes the
       last argument as the test would name the harness. */
    if (argc < 3) {
        fprintf(stderr, "usage: wpt_runner [--test-document] [--variant ?v] <testharness.js> <test> [more.js ...]\n");
        return 2;
    }
    /* Which file is the test, where it lives, and whether it is a document, derived before anything is built: an
       HTML test's document must be fetched and parsed before `document` is installed, and there is no second
       document to swap in later. */
    wpt_derive_addresses(argc, argv);
    /* The test document, built by the same call as a child document. It is this process's top-level traversable:
       the runner navigated to it and nothing embeds it, so its top-level creation URL is its own address. There
       is no creator, so §7.1.7 has no container to clone: the empty CSP pair says so (CSP §2.2.2's self-origin is
       this address's origin), and §7.1.4's item is "a new embedder policy", spelled out because an embedder
       policy has no absence. What the document's own response says is obtained inside the build, which fetches
       the test document itself in `--test-document` mode; a `.any.js` run has no document response (its skeleton
       is this runner's), so no bytes or headers are passed. §7.3.1.3's parent is `u`, §3.1.3's list is empty
       (no container document, step 3), and §7.1.5's set is empty (a top-level traversable's popup sandboxing
       flag set, which only §7.3.1.7's rules for choosing a navigable fill), and §7.2.2.4's opener is `N`, each in
       its grammar's own word so an empty field cannot be mistaken for a host that stopped stating it. */
    ctx = wpt_build_document("wpt", g_top_origin, g_base_url, NULL, 0, NULL, "", "", "",
                             "unsafe-none", "", "unsafe-none", "", FETCH_DEFAULT_REFERRER_POLICY, "u",
                             PERMISSIONS_POLICY_SERIALIZED_NO_CONTAINER,
                             DOCUMENT_ANCESTOR_ORIGINS_SERIALIZED_NONE,
                             SANDBOX_FLAGS_SERIALIZED_NONE, "N");
    /* The realm a relayed notice is delivered into. A child's notice arrives while this process is blocked inside
       wpt_child_ask, which has no ctx of its own. */
    g_wpt_ctx = ctx;
    rt = g_rt;

    /* The programs this run is, built before the session because a session is opened over them. */
    {
        if (g_html_mode) {
            /* The document's own scripts in document order, from the component the engine loads a real page with,
               so `<script src=/resources/testharness.js>` loads as the page asked. No prologue: `GLOBAL` is the
               shim WPT's server writes around a `.any.js`. An external script is fetched into its own slot, in
               position, because classic scripts run in document order.
               Named residual. Not covered: this path pre-fetches instead of using the engine's docscript park (the
               flow waits, wpt_issue_pending asks, and the reply carries the status §8.1.4.2's "fetch a classic
               script" step 5.2 refuses on), so a `<script src>` answered with a non-ok status is dropped from the
               sequence rather than run through §4.12.1.1 "Processing model"'s step 4 ("If el's result is null, then
               fire an event named error at el, and return"). Dropping fires nothing, so an `onerror` assertion fails
               rather than passing. Next diff builds: this loop deleted as the child path's was, which first needs
               the report hook's position derived from the document's script inventory, because wpt_insert_report
               searches `g_prog_names` for the harness and a parked row contributes no name. Absence shows as: a
               test that loads a script under `?pipe=status(404)` and asserts an `error` event reports it never
               firing, in html mode only, while the same assertion passes in a child document. */
            DocScripts ds = document_exec_scripts(g_wpt_dom);
            for (i = 0; i < ds.n; i++) {
                /* An inline `<script>` has no address. §4.12.1.1: "If el does not have a src content attribute:
                   Let base URL be el's node document's document base URL." The engine reads a NULL address as
                   exactly that. */
                if (ds.bodies[i]) {
                    wpt_program(strdup(ds.bodies[i]), g_test_url, /*address*/NULL, ds.types[i]);
                    continue;
                }
                if (!ds.srcs[i]) continue;
                {
                    size_t len = 0;
                    char *body = wpt_get(ds.srcs[i], &len);
                    /* A <script src> the server does not have is a real 404; a browser fires an error event and
                       carries on. Reporting it names the file, because a test whose harness failed to load
                       otherwise reports nothing. */
                    if (!body) {
                        fprintf(report_out(), "@WPTERR %s: <script src> did not load: %s\n",
                                g_test_url, ds.srcs[i]);
                        continue;
                    }
                    /* An external script's address is its own: §8.1.4.2 "Fetching scripts" creates the script with
                       "response's URL" as its base, and for a module that address is also the module map key, so
                       two `<script type=module src>` are two modules. The raw `src` attribute crosses as-is:
                       §4.12.1.1's encoding-parse is relative to el's node document, whose realm the engine holds. */
                    wpt_program(body, ds.srcs[i], ds.srcs[i], ds.types[i]);
                }
            }
            doc_scripts_free(&ds);
        } else {
            wpt_program(strdup(WPT_PROLOGUE), "<wpt-prologue>", /*address*/NULL, SCRIPT_TYPE_CLASSIC);
            /* The harness and the META scripts are program inputs the driver resolved, so they come from the paths
               it named (it fetched and substituted any `.sub.js` first). The test is the run's address, so it
               comes from the server. */
            for (i = 1; i < argc - 1 && !failed; i++) {
                size_t len = 0;
                char *src = read_file(argv[i], &len);
                if (!src) { fprintf(report_out(), "@WPTERR %s: cannot read\n", argv[i]); failed = 1; break; }
                /* No address: these bytes were read from a path on disk, not obtained from a URL, so §8.1.4.1's
                   base URL ("the URL from which the script was obtained") falls back to the document's address,
                   which a generated `.any.html` gives a same-directory helper anyway. */
                wpt_program(src, argv[i], /*address*/NULL, SCRIPT_TYPE_CLASSIC);
            }
            /* The test itself is fetched, as a document test is: a `.sub.` file is a template, and wptserve
               substitutes `{{host}}` and `{{ports[https][0]}}` when it serves one. Off disk,
               `dom/events/EventListener-addEventListener.sub.window.js` would open an iframe at a string that is
               not a URL. */
            if (!failed) {
                size_t len = 0;
                char *src = wpt_get(g_test_url, &len);
                if (!src) wpt_unserved_test_file();
                /* This one has an address: the bytes came off the corpus server at g_test_url, which is §8.1.4.1's
                   "URL from which the script was obtained" and what a relative `import('./helper.js')` in the test
                   resolves against. */
                wpt_program(src, g_test_url, g_test_url, SCRIPT_TYPE_CLASSIC);
            }
        }
        /* The report and the epilogue are programs of this document like the rest. The report registers where
           WPT's vendor hook goes, immediately after testharness.js, so no helper the test loads is ahead of it.
           The epilogue is the script arm's alone (see WPT_EPILOGUE): `.any.worker.js` carries `done()` and
           `.any.html` does not, and the arm above already declared which this run is. */
        if (!failed) {
            wpt_insert_report();
            if (!g_html_mode)
                wpt_program(strdup(WPT_EPILOGUE), "<wpt-epilogue>", /*address*/NULL, SCRIPT_TYPE_CLASSIC);
        }
    }

    /* The one scheduler, over this document's programs. A flow's jobs are its own queue, a parked continuation is
       resumed by flow_step before anything else the flow could do, and the document lifecycle, rendering
       opportunity and due timer are asked through the hooks their components registered, so this host keeps no
       pump or event-loop order of its own. Forking is off: this gate measures the browser half against a spec
       oracle, so a concolic branch must not explore both arms, as in the child half.
       No element column: these programs are flattened by wpt_program rather than collected as `<script>`
       elements, so §3.1.7's `currentScript` is null while each runs, that section's answer for a document not
       executing a script element. wpt_child_main passes document_exec_scripts's column. */
    /* The test document is the one this runner was named, as a person names a seed. */
    engine_set_arrival(engine_arrival_of_token("observed,undriven,unpinned"));
    engine_sched_begin(ctx, g_prog_bodies, g_prog_srcs, g_prog_types, /*sched*/NULL, /*els*/NULL, g_prog_n,
                       /*forking*/0, NULL);
    for (;;) {
        int r = engine_sched_step();
        int did;

        /* The step after a park request takes it (engine.h's engine_request_park: at the next boundary with no
           flow switched in, the top of the next slice), so the run is over when it returns. Still owed are the
           park's own announcements, emitted between writing the residue and closing the session. Requests are not
           answered: the session is closed, and engine_host_answer into a closed frontier delivers to nobody. */
        if (g_parking) {
            DCHECK(r == ENGINE_STEP_DONE,
                   "this host asked the engine to write its frontier out and the step that followed did not "
                   "take the park — the residue is still live, so the teardown below is about to release "
                   "flows whose recipes were never written");
            wpt_drain_notices(ctx);
            break;
        }

        /* The host's half of every slice boundary, asked at every one rather than only at a stall, as the
           extension's bridge does (engine.h): paying only at a stall would make one flow's reply wait for every
           other flow to block. Notices come first, because a document announced but not yet provisioned is one a
           read would arrive for before its instance exists. */
        did = wpt_answer_host_requests(ctx);
        if (wpt_issue_pending() > 0) did = 1;
        if (wpt_net_pump(ctx, /*block*/0) > 0) did = 1;
        /* After the host's half, not before: the final step emits like any other (a notice naming a document, a
           reply a flow was owed), and a DONE checked first would leave those undrained. */
        if (r == ENGINE_STEP_DONE) break;

        /* The measurement ended, so the session does (HTML §7.3.1.6 "Navigable destruction"; js_wpt_test_complete
           says why that is not a bound). The teardown below performs the destruction: every document this process
           holds is released, and each child navigable's process is reaped.
           It pages rather than breaks. Ending a session over a live frontier suspends only the running flow, and
           the other members would reach flow_registry_free holding parked continuations, frames, reactions and
           owed replies, which flow_release forbids: a session ending over live flows either paged them or had
           nothing to lose. engine_request_park writes every member out as a recipe (the operation the extension
           asks for at the RAM floor), and the step that takes it answers DONE with every flow paged. A conformance
           gate has no cross-session frontier to keep the residue in, which is a fact about this host's storage.
           It is asked below the DONE exit because the two ends are independent: the measurement ending, and the
           frontier draining, which closes the session inside that step. The epilogue is usually the last
           program, so one step often does both; asked above the exit, the park would target a closed session.
           Below it the session is live by construction, which both park requests in this loop rely on. */
        if (g_test_complete) { engine_request_park(); g_parking = 1; continue; }

        if (r == ENGINE_STEP_STALLED && !did) {
            /* Nothing runnable and nothing filled. A request in flight means the frontier is parked on the
               network, the ordinary state of a browser, so this host sleeps in `poll` until an answer lands; there
               is no timeout, since a deadline could only truncate a reply that was merely slow. A referenced
               document at rest is the other legitimate state: it created a navigable another instance hosts
               (engine.h's engine_set_referenced), so its last timeline waits to be asked something by a peer. With
               nothing in flight it waits on the children still in their boot round, the only channels that can
               write to it unasked, and once every child has rested nothing can arrive (trusted.mjs's all-stalled
               end) and it pages. Otherwise the scheduler says it is owed something and this host says it owes
               nothing, so the parked flows wait for a reply nobody will send; in release the run pages and ends. */
            int booting = wpt_children_booting();

            DCHECK(g_inflight_n > 0 || booting || engine_referenced(),
                   "the frontier stalled on work this host says it owes, this host filled NOTHING, and it has "
                   "NOTHING IN FLIGHT — the stall hook and the network edge are two answers to one question "
                   "and they disagree, so the flows parked here are waiting for a reply nobody will send");
            /* This end pages for the same reason, so the two ways this host stops are one operation. */
            if (!g_inflight_n && !booting) { engine_request_park(); g_parking = 1; continue; }
            wpt_net_pump(ctx, /*block*/1);
        }
    }
    /* The engine's own answer to the park's claim: engine_frontier_paged says whether this frontier was written
       rather than drained, checked where the claim is load-bearing. */
    DCHECK(!g_parking || engine_frontier_paged(),
           "this host asked the engine to page its frontier out and the engine does not report a frontier that "
           "was written — so the teardown below is about to release flows whose work has no recipe, which is "
           "the drop flow_release names one flow at a time");
    /* The session ends here, unconditionally. The loop has one exit, DONE, because the other ends ask for a park
       and the step that takes it answers DONE; engine.h makes this call harmless then, and the rule it obeys is
       "call it when you stop stepping", so a new exit cannot forget it. Which exits already closed the session
       is the engine's question. */
    engine_sched_end();

    /* The program sequence and the request records, which outlive the session by design and are owned by nothing
       else. The session borrowed the arrays (engine.h), so they are freed only now. A record still here is a
       request whose flow was parked when the run stopped, which the frontier's teardown asserts about from the
       other end. */
    {
        int k;
        for (k = 0; k < g_prog_n; k++) { free(g_prog_bodies[k]); free(g_prog_names[k]); free(g_prog_srcs[k]); }
        free(g_prog_bodies);
        free(g_prog_names);
        free(g_prog_srcs);
        free(g_prog_types);
        while (g_owed_n) wpt_owed_forget(g_owed_n - 1);
        wpt_net_free();
    }

    /* The frontier first, because a suspended flow is a live activation of the browser (solver/engine.h). A flow
       parked inside §8.1.4.6 step 6.2's `error` dispatch gives back its step 6.1 flag under a private Symbol that
       the platform release below frees. */
    solver_frontier_free(ctx);
    /* The platform's own list, undone in one call (see main.c's teardown). Every component of core/platform.h,
       including rendering, timers, message ports, event targets, the viewports, Location, History, the navigation
       API, the event loop record, Fetch's Headers, Response and Request, URL, FormData, the streams group,
       AbortSignal and Observable, Window, the cross-agent seam, Event, the File API, the File System model,
       XMLHttpRequest and Encoding, is a row on its release column, released in reverse declaration order, so a
       dependent is released before what it depends on. platform_check_agent_state fires on a row with agent
       state and no release. */
    platform_agent_free();
    navigable_free(ctx);
    /* Nothing switches the running flow out here: a session ends by closing (engine_session_close performs that
       switch-out, which makes the frontier a set of snapshots), and a flow still switched in now is a session
       that never closed, which flow_release reports. The solver's own list is one call, in solver/engine.h, for
       the reason the platform's is. */
    solver_agent_free(ctx);
    document_free(ctx);
    /* document_free releases a realm's record, not the agent's. The DOM group, document_agent_free among it, is a
       set of rows on core/platform.h's release column, and the two are not ordered against each other (see
       main.c's teardown). */
    realm_intrinsics_free();   /* the declarations are the agent's; each realm's prototypes went with it */
    /* After the frontier (solver_agent_free above), because a flow parked inside an IDL member reads this pool at
       its teardown; idl_args_free asserts the position. */
    idl_args_free(ctx);
    /* The child documents this file created. A child is a process: closing its pipes ends its serve loop and
       waitpid reaps it, a leak no GC walk could see because the thing leaked is not in this heap. */
    wpt_children_free();
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    /* After JS_FreeRuntime, and the order is part of the meaning: this releases step definitions, which
       JS_RegisterStepDef borrows and requires to outlive the runtime (JS_FreeRuntime's [stepleak] report reads
       `def->steps` to name each unfinished machine by its step). The IDL pool's blocks hold such definitions. */
    idl_args_pool_free();
    idl_async_iter_free();
    return failed ? 1 : 0;
}
