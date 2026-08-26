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
 * a conformance pass over a plain JS_Eval would test something that never runs.
 *
 * AND THE EXECUTION MODEL IT RUNS IS THE PRODUCT'S, WHICH IS THE STRONGER STATEMENT AND IS NOW TRUE OF THE
 * WHOLE FILE. Parking a frame is not the same thing as being scheduled: a `while (JS_FlowResume)` exercises the
 * park-and-rebuild machinery while the SCHEDULER never runs, so a gate built on one measures a model the
 * product does not have. Both documents this binary runs — the TEST document and the CHILD document it
 * re-executes itself for — are on the one scheduler: engine_sched_begin seeds the document's programs on the
 * frontier, engine_sched_step is the only thing that enters JS, engine_route attaches a delivery to every live
 * timeline, and engine_perform makes a peer's operation a program of each of them. What that deleted was a
 * `while (JS_FlowResume)` per program, a fabricated flow standing in for the scheduler's state, and a
 * `while (JS_ExecutePendingJob)` job drain beside a frontier whose flows own their own queues.
 *
 * WHAT PREEMPTS IS THEREFORE THE SCHEDULER'S POLICY AND NO LONGER THIS FILE'S. It used to force a yield at
 * every back-edge and hand it straight back at the next loop iteration, which exercised the suspend/rebuild
 * path and nothing else; that coverage is test262's, which measures ENGAGEMENT (§Testing) and is the harness
 * built for it. Here the value yield and the cooperative quantum decide, exactly as they decide in the
 * extension, because that is what this gate is for.
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
#include "core/mime/mime_type.h"   /* §4.2's essence: what this host states it served */
#include "core/url/url.h"
#include "core/html/html_parse.h"   /* the ONE place a Document is parsed — that header owns the token bytes */
#include "core/loader/document_load_type.h"  /* §7.4.5: WHICH document a response loads as, and its computed type */
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
#include "solver/result.h"   /* who prints a page error while the programs are the scheduler's */
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <poll.h>
#include <errno.h>
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
#include "core/geometry/dom_rect.h"
#include "core/geometry/dom_rect_list.h"
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
#include "core/dom/node_interface.h"   /* the ONE place a Document is made — see that header */

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

/* THE MEASUREMENT IS OVER WHEN THE HARNESS SAYS IT IS — see js_wpt_test_complete and the loop in main. */
static int g_test_complete;

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

/* THE END OF THE MEASUREMENT, STATED BY THE HARNESS ITSELF — the vendor half of testharness's completion.
 *
 * WHAT IT IS FOR. `done()` ENDS A WPT TEST: testharness sets its phase to COMPLETE, runs the completion
 * callbacks and reports nothing afterwards. WPT's own model is that the runner then takes the browsing context
 * away — HTML §7.3.1.6 "Navigable destruction"'s destroy-a-top-level-traversable, whose own note is that a
 * user agent "may destroy a top-level traversable at any time (typically, in response to user requests)". The
 * DESTROY is the right one of that section's algorithms and close/definitely-close is not: close fires
 * `beforeunload` over the subtree and unloads it, which is more of the page's program, and this page's program
 * has already been measured.
 *
 * IT IS NOT A BOUND, and the distinction is the whole of §NO BOUNDS rather than a nicety. A bound TRUNCATES
 * work that was still going to produce something — a step cap, a deadline, an idle counter — and cannot know
 * that it did. This truncates nothing that is measured: every subtest has a result, the file-level verdict has
 * been printed, and testharness will not accept another. What ran on past it was a poll loop waiting on a
 * remote context that never came up, which is a flow parked on a reply nobody was going to send, and the
 * SESSION ending is what that flow's snapshot is for.
 *
 * IT IS A CALL AND NOT A LINE OF OUTPUT ON PURPOSE. The @WPTDONE line is the DRIVER'S report and stdout is the
 * driver's channel; this host learning the same fact by reading back what it printed would be one fact with
 * two spellings, and the one it reads would be the one that can be forged by a test that calls `print`. The
 * report hook is registered where WPT puts the vendor hook (see WPT_REPORT), so a call out of its completion
 * callback is exactly the integration point testharnessreport.js exists to be. */
static JSValue js_wpt_test_complete(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)ctx; (void)this_val; (void)argc; (void)argv;
    DCHECK(g_report == NULL, "a CHILD document declared the measurement over — the file under test is the one "
                             "this process was started to run, and a child navigable's own harness ending "
                             "would take the parent's session down with a verdict nobody printed");
    DCHECK(!g_test_complete, "the harness declared this file complete TWICE — testharness reports completion "
                             "once and refuses results afterwards, so a second arrival is a second harness in "
                             "this process claiming to end a measurement that is not its own");
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


/* THE REPORT — WPT'S OWN VENDOR HOOK, AT WPT'S OWN POSITION, and both halves of that are the fix.
 *
 * WHAT WAS WRONG. The whole report hung off ONE completion callback appended at the END of the run, after the
 * test and after every `// META: script=` the test names. testharness.js's notify_complete is a bare
 * `forEach(this.all_done_callbacks, callback)` with no containment, so the FIRST of those callbacks that throws
 * takes every later one with it — and `phase` is already COMPLETE by then, so nothing retries. A corpus helper
 * that registers a completion callback therefore DELETED this gate's entire report for the file: no @WPT lines,
 * no @WPTDONE, a clean exit, and a driver that could only say "0 reported subtest(s)".
 * It is not hypothetical and it is not one file. `IndexedDB/resources/support.js` opens with an
 * add_completion_callback that closes each test's database and calls `indexedDB.deleteDatabase` — which is
 * honestly ABSENT in this engine (core/indexeddb/indexed_db.c says so by name), so it is a TypeError. Nineteen
 * IndexedDB files were destroyed exactly that way: the fifteen `idbcursor*` files that use `createdb`, plus
 * cursor-overloads, value, abort-in-initial-upgradeneeded and close-in-upgradeneeded — every IDB file whose
 * open succeeded far enough for support.js to record a `db` on the test. Those files RAN, their subtests had
 * real results naming real gaps, the harness COMPLETED, and the last statement of the run threw the answer away.
 * AND THE THROW ITSELF WAS INVISIBLE, which is why the file exited clean with no @WPTERR either: the completion
 * runs inside an event listener, and DOM §2.9 "Dispatching events" step 2.11 of inner invoke REPORTS a
 * listener's exception rather than propagating it (core/events/event_target.c hands it to report_exception_run,
 * which fires an ErrorEvent), while HTML §8.1.4.6 "Runtime script errors" step 7.3 — report it to a developer
 * console — has no console in this build. So nothing this driver reads ever saw it. The gate now reports the
 * harness's own status, which catches the case where that ErrorEvent lands BEFORE completion; a reported
 * exception reaching this host at all is step 7.3's, and belongs to that component.
 *
 * SO THE REPORT GOES WHERE WPT PUTS IT. Every testharness document loads `/resources/testharnessreport.js`
 * immediately after `/resources/testharness.js`, and that file exists for exactly this — "vendors to implement
 * code needed to integrate testharness.js tests with their own test systems", via add_result_callback and
 * add_completion_callback. Registered there, this runner's callbacks are FIRST in every list, so no helper the
 * test loads can reach them, and the position is the corpus's own convention rather than this file's guess.
 *
 * AND IT STREAMS, for the reason the driver's own area rows stream: a report that exists only at completion is a
 * report you do not get from a file that never completes. A subtest is printed when its RESULT is known, and a
 * subtest is announced when it is REGISTERED — so "0 subtests reported" and "0 subtests existing" stop being the
 * same output. What the completion callback still owns is the ones the harness forced COMPLETE without a result
 * (its own timeout path does that) and the file-level verdict. `i` is the harness's own `tests` index, which is
 * what makes a re-announced or re-reported subtest one fact rather than two. */
static const char WPT_REPORT[] =
    "(function () {\n"
    "  var announced = [], reported = [];\n"
    "  function emit(t) {\n"
    "    reported[t.index] = 1;\n"
    "    print('@WPT ' + JSON.stringify({ i: t.index, name: t.name, status: t.status, message: t.message }));\n"
    "  }\n"
    "  add_test_state_callback(function (t) {\n"
    /* The harness notifies test state at PUSH and again when the test STARTS. One line per subtest, because a
       corpus file can hold ninety thousand of them and this output is read through a fixed buffer. */
    "    if (announced[t.index]) return;\n"
    "    announced[t.index] = 1;\n"
    "    print('@WPTSTART ' + JSON.stringify({ i: t.index, name: t.name }));\n"
    "  });\n"
    "  add_result_callback(emit);\n"
    "  add_completion_callback(function (tests, status) {\n"
    "    for (var i = 0; i < tests.length; i++) if (!reported[tests[i].index]) emit(tests[i]);\n"
    "    print('@WPTDONE ' + JSON.stringify({ status: status.status, message: status.message,\n"
    "                                         count: tests.length }));\n"
    /* AND THE MEASUREMENT ENDS HERE, after the verdict is out — see js_wpt_test_complete. */
    "    wptTestComplete();\n"
    "  });\n"
    "})();\n";

/* THE EPILOGUE. Outside a browser testharness has to be TOLD the page is done — one line of the harness's own
   public API. It is all that is left here: the REPORT moved to where WPT's own vendor hook goes, because being
   the last thing registered is what let a corpus helper delete it. */
static const char WPT_EPILOGUE[] =
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


/* THE REQUEST THE FRONTIER'S OWN REGISTER CANNOT HAND BACK — this file's half of the reply seam, and the exact
 * shape of the capability that is still missing rather than a convenience.
 *
 * A `fetch()` PARKS the flow that issued it (engine_pending_fetch_url): the flow keeps its snapshot, reports
 * itself host-owed, and its continuation resumes with the reply. That register records the WHOLE request —
 * method, headers and body — and the seam the HOST is offered now names TWO of those: engine_pending_fetches
 * lists `METHOD<TAB>URL` and engine_provide delivers against the pair. The corpus still asks this host for
 * POSTs whose answer depends on the BODY sent (`echo-content.py`) and for probes whose answer is the HEADERS it
 * was given (`inspect-headers.py`), and neither is on the line.
 *
 * SO THE HOST STILL KEEPS ITS OWN COPY, keyed by the pair the seam does name, and CRASHES where that key is not
 * enough: two outstanding requests with one method and one URL that differ in BODY are two questions the seam
 * cannot tell apart, and answering either with the other's reply is a wrong answer with nothing to say so. The
 * method half of that assert is GONE because the seam carries it — it is the identity now, not a collision.
 * What remains is the body (and the headers it rides with), and the same argument that moved the method moves
 * them: the line grows an identity for the whole request, and this table deletes with it.
 * The strings are this runner's copies, because the request the component built is gone by the time the
 * scheduler stalls on it. */
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
    /* BOTH HALVES ARE THE COMPONENT'S TO STATE, and this host used to fill in either. `"GET"` for an unstated
       method was a real value belonging to some other request — the exact hole the pair seam was built to
       close — and `""` for an unstated URL parks a flow on an address no host can ever answer. */
    const char *method = req->method;
    const char *url = req->url;
    int i;

    DCHECK(method && *method && url && *url,
           "the wpt provider was owed a request that does not state its METHOD and URL — the reply seam is "
           "keyed on the pair (solver/engine.h), so a request missing either cannot be listed, fetched or "
           "delivered. The component that built it must state both");

    for (i = 0; i < g_owed_n; i++) {
        /* THE TABLE IS KEYED ON WHAT THE SEAM IS KEYED ON — the pair. A GET and a POST to one address are two
           rows here because they are two lines on the join and two deliveries at the provide. */
        if (strcmp(g_owed[i].url, url) || strcmp(g_owed[i].method, method)) continue;
        /* THE SAME REQUEST FROM ANOTHER FLOW is the ordinary case — a candidate re-fire re-runs the fetches the
           exploring flow made, and the frontier answers all of them with one reply. A DIFFERENT BODY under one
           (method, url) is the seam's remaining limit above, and it is fatal rather than first-come-first-served. */
        DCHECK(g_owed[i].body_len == req->body_len &&
               (req->body_len == 0 || (g_owed[i].body && req->body &&
                                       !memcmp(g_owed[i].body, req->body, req->body_len))),
               "two outstanding requests share one (method, url) and differ in BODY — the reply seam names the "
               "pair and not the body, so whichever is answered first answers both and one flow resumes with "
               "the reply to a question it never asked. Put the whole request's identity on the line");
        /* AND THE PARK, which is what this provider does with the flow: it waits on its own register, exactly
           as it does in the extension. */
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

static void wpt_send_all(int fd, const char *p, size_t n)
{
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w <= 0) return;
        p += w; n -= (size_t)w;
    }
}

/* WHAT THIS HOST COMPUTED THE RESOURCE TO BE (core/fetch/fetch.h). This runner IS the network side for the
   corpus — it holds the socket and reads the bytes — so it is the zone entitled to answer, exactly as
   `extension/lib/safe-fetch.js` is in the extension. wptserve serves the checked-out corpus with real types
   and states them, so the answer is §4.2's ESSENCE of the `Content-Type` it sent; a response that named none
   answers the EMPTY string, which is §5.1's "the supplied MIME type is undefined" and a value rather than a
   hole. Malloc'd; the caller frees. */
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

/* ---- THE NETWORK EDGE IS A SET OF REQUESTS IN FLIGHT, NEVER A CALL THAT WAITS ---------------------------
 *
 * WHAT WAS HERE. One `read(2)` loop per request, run from inside the step loop. The FLOW that issued a fetch
 * parked correctly — the engine's own register held it — and then this host stood in a blocking read with the
 * WHOLE FRONTIER behind it: the other timelines of this document, the deliveries a child instance had already
 * written up its pipe, the due timers, and the very flow whose POST would have satisfied whatever the answer
 * was waiting on. That is §scheduler's cardinal violation performed by the HOST rather than by the engine — a
 * wait that runs to completion and cannot be suspended — and where the answer depends on anything this process
 * would have done next it is a deadlock by construction, because the thread that owes the answer is the thread
 * that is blocked waiting for it.
 *
 * MEASURED, on the live process: `back-forward-cache/events.html` sat in this read for the gate's whole 600 s
 * wall backstop having consumed 0.00 s of CPU — subtests already reported, frontier frozen mid-poll, the
 * backtrace `main → wpt_provide_pending → wpt_http → read(3)`. Nine files in `html/browsers` did the same,
 * which is an hour and a half of every area run spent asleep in one syscall.
 *
 * WHAT IT IS NOW. A request is BEGUN and this host returns to the loop; its socket is a row in the table below
 * and the flow that asked stays parked on the engine's register exactly as it already did. This is the SAME
 * park, paid the way the extension pays it (engine.h's `qjs_pending` → fetch → `qjs_provide` pair, whose
 * network side is a Promise and has never blocked a step) — not a second wait primitive beside it. Every slice
 * boundary drains whatever arrived and delivers it; the flows that can run, run.
 *
 * THERE IS NO TIMEOUT, NO POLL BUDGET AND NO RETRY COUNT, and that is not an omission — §NO BOUNDS. A reply
 * that never comes leaves its flow parked for ever, which is the correct state: the frontier runs everything
 * else, and the only thing a deadline could do is truncate a reply that was merely slow. Where the frontier
 * has NOTHING runnable and requests are outstanding, this host has no work of its own left, so it blocks in
 * `poll` with an INFINITE timeout — a thread with nothing to do is not a bound, and that wait ends the moment
 * any one of the answers lands.
 *
 * AND THE CONNECTION IS THE BROWSER'S, NOT ONE PER REQUEST. A socket opened, used once and closed with
 * `Connection: close` is not a cheaper persistent connection, it is a DIFFERENT PROTOCOL — RFC 9112 §9.3
 * "Persistence" makes HTTP/1.1 persistent by DEFAULT and §9.6 "Tear-down" says a client that sends the `close`
 * option MUST NOT send another request on that connection. What that costs is not throughput: wptserve is a
 * ThreadingHTTPServer, so every connection is a THREAD, and every thread makes its own multiprocessing-manager
 * connection to the stash. One file's poll loop opened 865 of them in 40 s and left 1251 sockets in TIME_WAIT,
 * and it was under exactly that churn that a handler desynchronised against the stash manager WHILE HOLDING
 * `dispatcher.py`'s lock — the server-side half of the wedge this runner spends its wall clock inside. A
 * browser holds the connection open and the churn does not exist.
 *
 * SO THE POOL IS KEYED ON THE TARGET URI'S AUTHORITY, which is the key a browser pools on and not the socket
 * address: every corpus authority resolves to the one loopback port here (the Host header carries the real
 * one, which is what a hosts-file mapping does for a real browser), and pooling on the ADDRESS would put two
 * origins on one connection. There is no per-origin connection limit, because a limit is a bound: a request
 * that finds no idle connection for its authority opens one.
 *
 * READING TO EOF IS THEN NO LONGER A FRAMING, AND THAT IS THE REAL CHANGE. `Connection: close` made the body
 * "whatever arrives before the socket dies"; a persistent connection has the NEXT response behind this one, so
 * the length has to be READ — RFC 9112 §6.3 "Message Body Length" in its own order of precedence, which is the
 * state machine below: §6.3 rule 1 (a HEAD response, or 1xx/204/304, has no body WHATEVER its header fields
 * say — get this wrong and the reader waits for ever on a `Content-Length` the server will never send bytes
 * for), rule 4 with §7.1 "Chunked Transfer Coding" and §7.1.3 "Decoding Chunked", rule 6's `Content-Length`,
 * and rule 8's close-delimited body for a response that declares neither. Rule 8 is not a fallback: it is a
 * FRAMING, and it says this connection cannot carry another request, which is how wptserve serves every static
 * file (its FileHandler hands the response an open file object rather than bytes, so no `Content-Length` is
 * written and the server closes) and why persistence is a property of each RESPONSE rather than of the pool. */
enum { WPT_REQ_SEND, WPT_REQ_READ, WPT_REQ_DONE };
/* RFC 9112 §6.3 "Message Body Length" AS THE STATES ITS READER PASSES THROUGH — one state per way a body can
   be delimited, plus the ones §7.1's chunked grammar needs. A reader that is in NONE of them has not decided
   the framing, which is the assert at the bottom of this block. */
enum { WPT_MSG_HEAD, WPT_MSG_LENGTH, WPT_MSG_CHUNK_SIZE, WPT_MSG_CHUNK_DATA, WPT_MSG_CHUNK_CRLF,
       WPT_MSG_TRAILER, WPT_MSG_CLOSE, WPT_MSG_COMPLETE };
/* WHAT A COMPLETED REQUEST IS DELIVERED AS. The four kinds differ in the ANSWER they build and in whom they
   hand it to, and in NOTHING else — one HTTP implementation with four consumers, never four network edges.
   SYNC is the one with no flow behind it: the document this process was started to run, and the external
   scripts of it, are fetched BEFORE there is a session, so there is nothing to park and nothing to starve. */
enum { WPT_DELIVER_SYNC, WPT_DELIVER_FETCH, WPT_DELIVER_DOCUMENT, WPT_DELIVER_XHR };

#define WPT_INFLIGHT_MAX 64
#define WPT_CONN_MAX 64

/* ONE TCP CONNECTION, HELD OPEN. `owner` is the request currently on it and 0 when it is idle — this runner
   does not PIPELINE (RFC 9112 §9.3.2 "Pipelining" permits it and no browser does it), so one connection
   carries one message at a time and §9.2 "Associating a Response to a Request"'s ordered outstanding-request
   list is a single slot. `in` is the octets read from the socket and NOT YET CONSUMED by a message: it belongs
   to the CONNECTION because framing does — what is left in it after a message ends is the beginning of the
   next one, and with no pipelining that is exactly nothing, which is what its own assert says. */
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
    char    *method, *url;     /* the pair engine_provide is keyed on, and the address an answer reports */
    char    *authority;        /* which pool this request draws its connection from */
    uint64_t conn;             /* the connection carrying it; 0 before it has one */
    int      reused;           /* it was put on an ALREADY-OPEN connection — see §9.3.1 Retrying Requests */
    uint32_t id;               /* DOCUMENT/XHR: the request id engine_host_answer names */
    char    *out;              /* the whole request, serialized ONCE at begin so nothing it borrowed has to
                                  outlive this call — the ownership half of making a call a work item */
    size_t   out_n, out_off;
    /* THE REPLY, PARSED AS IT ARRIVES rather than split out of one buffer at the end: with the connection held
       open there is no "the end", so the header section is parsed the moment it is complete and the body is
       accumulated by whichever of §6.3's framings the header section chose. */
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

/* IS THIS QUESTION ALREADY ASKED. The pending registers list an entry until it is ANSWERED, so a host that
   issued from them at every slice boundary would issue the same request once per slice — a page's one `fetch`
   becoming an unbounded stream of them. The key is the one the delivery is keyed on, which is why these are
   two functions and not one: a network park is `(method, url)` (engine.h) and a host request is its id. */
static bool wpt_request_asked(const char *method, const char *url)
{
    int i;
    for (i = 0; i < g_inflight_n; i++)
        if (g_inflight[i].deliver == WPT_DELIVER_FETCH &&
            !strcmp(g_inflight[i].method, method) && !strcmp(g_inflight[i].url, url)) return true;
    return false;
}

static bool wpt_request_asked_id(uint32_t id)
{
    int i;
    for (i = 0; i < g_inflight_n; i++)
        if (g_inflight[i].deliver != WPT_DELIVER_FETCH && g_inflight[i].deliver != WPT_DELIVER_SYNC &&
            g_inflight[i].id == id) return true;
    return false;
}

/* Drop one connection and forget its buffered octets. A request still on it is NOT failed here — the caller
   that killed the connection decides whether this was the end of a message, a network error or a retry. */
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

/* A CONNECTION FOR THIS AUTHORITY. An idle open one is REUSED, which is the whole point; `force_new` is asked
   only by §9.3.1's retry, where reusing anything is precisely the thing that just failed. */
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

    /* THE POOL IS FULL AND AN IDLE CONNECTION IS A CACHE ENTRY, so one goes to make room. This is not a limit
       on work — nothing is queued, dropped or deferred; a held-open socket for an authority nobody is asking
       about right now is a saving, and giving one up costs the next request to that authority a connect. What
       cannot be given up is a connection carrying a request, which is why the CHECK below is still fatal: past
       that point this runner would have to hold a request it cannot send. */
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

/* Begin one request against the corpus server and RETURN, whatever stage it reaches. The socket is
   non-blocking from before the connect, so not one of the stages below can hold this thread. */
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
    CHECK(r->method && r->url, "wpt: OOM recording a request in flight");

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
    /* AN ADDRESS THAT DOES NOT PARSE IS A NETWORK ERROR, and it is DELIVERED as one rather than dropped: the
       flow that asked is parked on this pair, so a silent return would park it for the life of the run. */
    if (!path) { free(authority); r->failed = 1; r->state = WPT_REQ_DONE; return r->seq; }
    r->authority = strdup(authority ? authority : "127.0.0.1");
    CHECK(r->authority != NULL, "wpt: OOM recording a request's authority");

    hn = strlen(path) + strlen(r->authority) + 512
       + (req->headers ? (size_t)req->headers->n * 512 : 0) + (req->body ? req->body_len : 0);
    r->out = malloc(hn);
    CHECK(r->out, "wpt: OOM building a request");
    /* NO `Connection` FIELD AT ALL, which is RFC 9112 §9.3 "Persistence"'s own default for HTTP/1.1 and not an
       omission: `close` would forbid a second request on this connection (§9.6 "Tear-down") and `keep-alive`
       is HTTP/1.0's mechanism for asking for what 1.1 already gives. */
    n = (size_t)snprintf(r->out, hn, "%s %s HTTP/1.1\r\nHost: %s\r\n",
                         req->method ? req->method : "GET", path, r->authority);
    for (i = 0; req->headers && i < req->headers->n; i++)
        n += (size_t)snprintf(r->out + n, hn - n, "%s: %s\r\n",
                              req->headers->e[i].name, req->headers->e[i].value);
    n += (size_t)snprintf(r->out + n, hn - n, "Content-Length: %zu\r\n\r\n", req->body ? req->body_len : 0);
    /* `snprintf` ANSWERS THE LENGTH IT WOULD HAVE WRITTEN, so an accumulated `n` that reached the end of the
       buffer would run PAST it and make every `hn - n` below underflow to a huge size_t. The sizing above is
       generous; this is what says so where a header list could outgrow it. */
    CHECK(n < hn, "wpt: a request outgrew the buffer sized for it — the header list is larger than the "
                  "allowance above, and the writes past this point would be counted against a negative size");
    if (req->body && req->body_len) { memcpy(r->out + n, req->body, req->body_len); n += req->body_len; }
    r->out_n = n;
    free(path);
    free(authority);

    wpt_request_attach(r, /*force_new*/0);
    return r->seq;
}

/* ---- READING ONE RESPONSE: RFC 9112 §6.3 "Message Body Length" ------------------------------------------ */

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

/* A BODY OF ZERO OCTETS IS A BODY, and this is what says so. A reply is turned into `fetch`'s network error by
   having NO body at all (wpt_reply_value answers JS_NULL for one), so a 200 with `Content-Length: 0` — which is
   what half the corpus's `.py` handlers answer — must arrive here with an empty buffer rather than a null one.
   Called once the status line has been accepted, which is the first moment there is a response to have one. */
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

/* THE HEADER SECTION, and then §6.3's ORDER OF PRECEDENCE over it. `head` is the whole section including its
   terminating CRLF. Answers whether the reader may continue; a false answer has already failed the request. */
static bool wpt_message_head(WptRequest *r, char *head, size_t hn)
{
    char *line, *sep = head + hn - 4;
    char *cl, *te, *conn;

    /* THE STATUS LINE. A version this runner cannot recognise is not a status — the octets on this connection
       are then not an HTTP/1 message at all and nothing below can be computed from them. */
    if (hn < 12 || memcmp(head, "HTTP/1.", 7) != 0) { r->failed = 1; return false; }
    r->http_minor = head[7] - '0';
    r->status = atoi(head + 9);
    wpt_body_init(r);

    header_list_free(&r->hdrs);   /* zeroes it — a 1xx below comes back here for the final response */
    /* EVERY FIELD LINE, AND THE LAST ONE IS A FIELD LINE. The search for a line's terminating CR ran to
       `sep - 1` and so EXCLUDED `sep` itself — which is precisely the CR of the last header, because the
       "\r\n\r\n" this section ends with IS that field's own terminator followed by the empty line. So the
       final field of every response was silently dropped, and the octets it carried came back as an absence:
       `wpt_computed_type` answered §5.1's "undefined" for a reply whose Content-Type was last, and the moment
       the connection stopped being closed after each response the same hole became a FRAMING one — a dropped
       `Content-Length` is §6.3 rule 8, so the reader waits for a hang-up on a connection the server is holding
       open. The count is `sep - line` because the range wanted is [line+1, sep] inclusive. */
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

    /* §6.3 rule 1: a HEAD response and a 1xx/204/304 response are terminated by the first empty line after the
       header fields REGARDLESS of the header fields present. This is not an optimisation — wptserve answers a
       HEAD with the `Content-Length` its GET would have carried and no body, so a reader that believed the
       field would wait for octets that are never sent, on a connection that stays open, for ever. */
    if (!strcmp(r->method, "HEAD") || r->status == 204 || r->status == 304) {
        r->msg = WPT_MSG_COMPLETE;
        return true;
    }
    /* §9.2 "Associating a Response to a Request": one or more informational responses may PRECEDE the final
       one. A 1xx has no body (rule 1), so the whole of it is these header fields and the reader goes back for
       the response the request is actually waiting on. */
    if (r->status >= 100 && r->status < 200) {
        r->msg = WPT_MSG_HEAD;
        return true;
    }
    /* §6.3 rule 2 is the CONNECT tunnel. This runner speaks to the corpus server directly and issues no
       CONNECT, so reaching it would mean the request record and the wire disagree about the method. */
    DCHECK(strcmp(r->method, "CONNECT") != 0,
           "a CONNECT reply reached this runner's message reader — RFC 9112 §6.3 Message Body Length rule 2 "
           "makes the octets after such a response a TUNNEL rather than a body, and this host has no proxy to "
           "tunnel through, so the request that produced it was built by something that must not have");

    te = header_list_get(&r->hdrs, "transfer-encoding");
    cl = header_list_get(&r->hdrs, "content-length");
    if (te) {
        /* §6.3 rule 3: Transfer-Encoding OVERRIDES Content-Length, and a message carrying both "ought to be
           handled as an error". It is one here: the corpus server sends one or the other. */
        if (cl) { free(te); free(cl); r->failed = 1; return false; }
        /* §6.3 rule 4 with §7.1 "Chunked Transfer Coding". A coding this host cannot decode is not a body it
           can hand on — the octets are the CODED form — so it crashes at the read rather than delivering a
           document that parses and is not what the server sent. */
        if (!wpt_field_has_token(te, "chunked") || strchr(te, ',')) {
            free(te);
            DFAIL("a corpus reply named a transfer coding this host does not decode — RFC 9112 §6.3 Message "
                  "Body Length rule 4 frames the body by that coding, so the octets after the header section "
                  "are the CODED form and handing them on would be a short document that parses; build the "
                  "coding in this file's message reader (§7.2 Transfer Codings for Compression names them)");
            /* IN A RELEASE BUILD THE LINE ABOVE IS NOTHING, so the coding is a NETWORK ERROR rather than a
               body decoded by the wrong coding — the same answer §6.3 rule 5 gives for framing that cannot be
               determined, and never the coded octets handed on as content. */
            r->failed = 1;
            return false;
        }
        free(te);
        r->msg = WPT_MSG_CHUNK_SIZE;
        return true;
    }
    if (cl) {
        /* §6.3 rule 6: a VALID Content-Length is the body length. Rule 5 makes an INVALID one an unrecoverable
           framing error at which a user agent MUST close the connection and DISCARD the response — which is
           the network error this delivers, never a body cut where the digits stopped. Rule 5's one exception
           is a comma-separated list whose values are all valid and all THE SAME, which is the shape a field
           sent twice arrives in (header_list_get joins repeats with ", "). */
        const char *p = cl;
        unsigned long long v = 0;
        int first = 1, bad = 0;
        for (;;) {
            unsigned long long w = 0;
            const char *d;
            while (*p == ' ' || *p == '\t') p++;
            d = p;
            while (*p >= '0' && *p <= '9') {
                /* A LENGTH THIS MACHINE CANNOT REPRESENT IS NOT A LENGTH. §7.1 says the same of a chunk size:
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
            /* §7.1.3 "Decoding Chunked": chunk-size is 1*HEXDIG, followed by an OPTIONAL chunk-ext (§7.1.1,
               which a recipient MUST ignore) and CRLF. A size that is not hex is a DECODING FAILURE, which
               §8 "Handling Incomplete Messages" makes an incomplete message and therefore a network error —
               `fetch/api/resources/bad-chunk-encoding.py` sends exactly that on purpose. */
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
                   errors due to integer conversion overflows" — an overflowed size would frame a LATER part of
                   the stream as this chunk's data, which is the smuggling shape §11.2 warns about. */
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
            /* §7.1.2 "Chunked Trailer Section", terminated by the empty line §7.1's chunked-body ends with. A
               recipient MAY discard the trailer fields and this one does — §7.1.2 forbids merging one into the
               header section unless that field's own definition says how. */
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

/* THE PEER HUNG UP. §6.3 rule 8's framing ENDS here; every other framing is §8 "Handling Incomplete
   Messages" — a body shorter than its Content-Length, or a chunked stream with no zero-sized chunk, is
   INCOMPLETE and is delivered as the network error it is, never as the short body that arrived. */
static void wpt_message_eof(WptRequest *r)
{
    if (r->msg == WPT_MSG_CLOSE) { r->msg = WPT_MSG_COMPLETE; return; }
    if (r->msg != WPT_MSG_COMPLETE) r->failed = 1;
}

/* Move one connection, and the request on it, as far as they can go WITHOUT blocking, and no further. */
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
        /* AN IDLE CONNECTION THAT HAS SOMETHING TO SAY. §9.2 "Associating a Response to a Request": data on a
           connection with no outstanding request is not a response and the client closes. In practice this is
           the server hanging up a connection whose last response was close-delimited, and dropping it here is
           what keeps a stale one from being handed to the next request. */
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
                /* §9.3.1 "Retrying Requests": a connection can be closed at any time, and a REUSED one that
                   died before it carried any of this response died for a reason that has nothing to do with
                   this request. It is re-sent on a connection this call opens, so the condition cannot hold
                   twice — this is a state, not a counter, and there is no retry budget anywhere. */
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
                /* §9.3.1 again, at the other end: the reused connection was closed before one octet of the
                   response arrived, so nothing about this request has been answered yet. */
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
        /* THE MESSAGE ENDED WITH THE CONNECTION STILL OPEN — which is the whole point of §9.3's persistence.
           What is left in the buffer is the beginning of the next response, and with no pipelining there is no
           next response, so anything left is a length this reader computed wrong.
           A FRAMING ERROR TAKES THE CONNECTION WITH IT: §6.3 rule 5 and §8 both say the recipient closes,
           because after a message whose end nobody could find, every later octet on it is unattributable. */
        r->state = WPT_REQ_DONE;
        c->owner = 0;
        r->conn = 0;
        if (r->failed || !r->persist || c->in_n) {
            /* §9.2 "Associating a Response to a Request": once octets on a connection cannot be attributed to a
               request, "message delimitation is now ambiguous" and the client closes — so residue takes the
               connection with it in EVERY build, and the assert below is what names the cause in a dev one. */
            DCHECK(r->failed || !r->persist,
                   "a reply ended with octets left on its connection and no request behind it — this runner "
                   "does not pipeline, so either the reader above stopped SHORT of where §6.3 Message Body "
                   "Length puts the end of this body, or the server sent more than the framing it declared");
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
    free(r->method); free(r->url); free(r->authority); free(r->out); free(r->body);
    header_list_free(&r->hdrs);
    g_inflight[i] = g_inflight[--g_inflight_n];
}

/* THE REQUESTS STILL OUTSTANDING WHEN THE RUN ENDS — each one a flow that parked and never resumed, which the
   frontier's own teardown asserts about from the other end — and then the connections they were held open on.
   Released here because a socket is not in the runtime's heap and no gc walk can see it. */
static void wpt_net_free(void)
{
    while (g_inflight_n) wpt_request_release(g_inflight_n - 1);
    while (g_conn_n) wpt_conn_release_at(g_conn_n - 1);
}

/* §4.1's clone of the REQUEST's URL list. This runner serves the checked-out corpus and follows no redirect, so
   the list is the one URL it was asked for — which is what makes `response.url` the address the test fetched
   and `response.redirected` false, both computed rather than declared. */
static JSValue wpt_reply_value(JSContext *ctx, WptRequest *r, int status, HeaderList *rh,
                               char *body, size_t len)
{
    char *cty = wpt_computed_type(rh);
    const char *url = r->url;
    JSValue v = body ? fetch_reply_new(ctx, status, "", rh, body, len, &url, 1, cty) : JS_NULL;
    free(cty);
    return v;
}

/* ONE COMPLETED REQUEST, HANDED TO WHOEVER PARKED ON IT. Answers how many registers it filled, which is what
   tells a stall this host answered from one it could not. */
static int wpt_request_finish(JSContext *ctx, WptRequest *r)
{
    size_t len = r->failed ? 0 : r->body_n;
    char *body = r->failed ? NULL : r->body;
    int status = r->failed ? 0 : r->status;
    int filled = 0, i;

    DCHECK(ctx != NULL, "a reply completed for a flow before this host had a realm to deliver it into — only "
                        "the pre-session loads run without one, and none of them is delivered");
    /* A NETWORK ERROR HAS NO HEADER LIST. What is on the request at this point is whatever arrived before the
       framing failed, and §2.2.5's network error is a response with none — handing a page half a header
       section beside a null body would be two answers about one reply. */
    if (r->failed) header_list_free(&r->hdrs);
    /* WHERE THE FRAMING IS READ, WHICH IS WHERE IT MUST HAVE BEEN DETERMINED. A reader that did not reach
       COMPLETE has not been told by RFC 9112 §6.3 Message Body Length where this body ends, so the octets
       below are a PREFIX of one — and a prefix of a document parses. */
    DCHECK(r->failed || r->msg == WPT_MSG_COMPLETE,
           "a corpus reply is being delivered by a reader that never reached the end of its message — §6.3 "
           "Message Body Length decides where a body ends and this one is a prefix, which parses as a short "
           "document rather than reporting anything");
    if (r->deliver == WPT_DELIVER_DOCUMENT) {
        /* THE ANSWER IS `{body, headers}`: a navigated Document is created from THIS response, so everything
           §7.5.1 reads off a header list belongs in it. The block is the same field-line form qjs_init takes,
           serialized by the component that parses it so the two cannot disagree.
           §2.2.5's BODY IS A BYTE SEQUENCE and this answer carries one as one — `JS_NewStringLen` would be
           quickjs's own UTF-8 decode, run by the zone that FETCHED on bytes HTML's encoding sniffing has not
           looked at yet. */
        JSValue v = JS_NewObject(ctx);
        char *field_lines = header_list_field_lines(&r->hdrs);
        JS_SetPropertyStr(ctx, v, "body",
                          body ? JS_NewArrayBufferCopy(ctx, (const uint8_t *)body, len) : JS_NULL);
        JS_SetPropertyStr(ctx, v, "headers", JS_NewString(ctx, field_lines));
        free(field_lines);
        /* THE HOST'S OWN ANSWER, so a NORMAL completion: this zone fetched bytes, it did not run a peer's
           program, and a load that did not load is `{body: null}` rather than a throw. */
        engine_host_answer(ctx, r->id, NULL, v, ENGINE_COMPLETION_NORMAL, ENGINE_ANSWER_HOST);
        JS_FreeValue(ctx, v);
        filled = 1;
    } else {
        JSValue reply = wpt_reply_value(ctx, r, status, &r->hdrs, body, len);
        if (r->deliver == WPT_DELIVER_XHR) {
            /* Also the host's own — XHR §3.5.6's fetch, answered out of the network; a network error is a
               reply that component reads, never a thrown value. */
            engine_host_answer(ctx, r->id, NULL, reply, ENGINE_COMPLETION_NORMAL, ENGINE_ANSWER_HOST);
            filled = 1;
        } else {
            filled = engine_provide(ctx, r->method, r->url, reply);
            /* THE REQUEST RECORD THIS ANSWERED, released only now: it holds the body and headers that made
               the question, and a second flow parking the same pair before the answer landed is the case its
               own DCHECK is there to catch. */
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

/* THE HOST'S HALF OF ONE SLICE BOUNDARY: move every connection as far as it goes, deliver the requests that
   finished, and answer how many registers that filled.
   `block` is asked ONLY where the frontier has nothing runnable and this host has nothing else to pay — see
   the loop in main. It is an INFINITE poll and never a deadline: §NO BOUNDS. */
static int wpt_net_pump(JSContext *ctx, int block)
{
    struct pollfd pfd[WPT_CONN_MAX];
    uint64_t map[WPT_CONN_MAX];
    int nf = 0, owned = 0, i, delivered = 0;

    for (i = 0; i < g_conn_n; i++) {
        WptRequest *r = g_conn[i].owner ? wpt_request_find(g_conn[i].owner) : NULL;
        pfd[nf].fd = g_conn[i].fd;
        /* WHAT EACH CONNECTION IS WAITING FOR. An IDLE one is watched for READ because the only thing it can
           produce is the hang-up §9.2 says to close on — never for WRITE, which is always ready and would
           spin. */
        pfd[nf].events = (short)(g_conn[i].connecting || (r && r->state == WPT_REQ_SEND) ? POLLOUT : POLLIN);
        pfd[nf].revents = 0;
        map[nf++] = g_conn[i].id;
        if (r) owned++;
    }
    DCHECK(!block || owned > 0, "this host was asked to WAIT with no request outstanding on any connection — a "
                                "poll with nothing to wait for is a hang, and the caller that asked for it "
                                "believes an answer is coming that nobody is going to send");
    if (nf) {
        int rc = poll(pfd, (nfds_t)nf, block ? -1 : 0);
        CHECK(rc >= 0 || errno == EINTR, "wpt: poll over the corpus connections failed");
        /* BY ID, NOT BY INDEX: advancing one connection can release it (a hang-up, a §9.3.1 retry) and the
           pool compacts, so the slot this row named may now hold a different connection entirely. */
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

/* Perform one request and return its BODY, or NULL — THE ONE WAIT LEFT IN THIS FILE, and it is not the one
   that was deleted. It is reachable only BEFORE the session exists: the document this process was started to
   run, and the external scripts of it. There is no flow to park, no frontier to starve and nothing else this
   thread could be doing, so waiting is the whole of the work. It is the SAME request machine as everything
   above — one HTTP implementation, entered here in "wait for this one" mode rather than a second one kept
   beside it. A non-2xx is a body like any other; a NULL body is what the delivery turns into `fetch`'s
   TypeError, the same answer a real network error gives. */
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
    if (body) r->body = NULL;                      /* MOVED to the caller, which frees it */
    if (phdrs && !r->failed) { *phdrs = r->hdrs; memset(&r->hdrs, 0, sizeof r->hdrs); }
    for (i = 0; i < g_inflight_n; i++) if (g_inflight[i].seq == seq) { wpt_request_release(i); break; }
    return body;
}

/* ISSUE EVERY REPLY THE FRONTIER IS PARKED ON — the host's half of one slice boundary, and it is the same
 * operation the extension's `qjs_pending`/`qjs_provide` pair performs: read what the flows are waiting for and
 * ASK for it. It does not wait, and that is the whole of the change: the answer arrives at wpt_net_pump, on
 * whichever later slice boundary the bytes land, exactly as the extension's reply arrives when its Promise
 * settles. Returns how many requests it started, which is progress this host made and therefore not a stall.
 *
 * IT DOES NOT ENTER JS. The delivery used to be `JS_CallAsFlow(deliver, reply)` out of the host's own time —
 * a call root outside any scheduled flow, settling a promise whose reactions then belonged to whichever flow
 * happened to be switched in. Now the resolve capability rides the flow's OWN pending entry: engine_provide
 * lands the reply there, the flow's next step drains it, and the reaction is enqueued on the flow that issued
 * the request. That is the whole reason the park is the product's park.
 *
 * A LINE WITH NO RECORD IN THE TABLE IS A REQUEST THE ENGINE ISSUED FOR CODE — an external document script, an
 * injected <script src>, a dynamic `import()` — and its METHOD IS ON THE LINE like every other. It used to be
 * defaulted to "GET" here, which was true and was still the wrong shape: the engine states the method at each
 * of those parks (engine.c) and the join carries it, so nothing on this side has to know which kind of park a
 * line came from. What the missing record does mean is that there are no headers and no body to re-send. */
static int wpt_issue_pending(void)
{
    char *list, *p;
    int issued = 0;

    if (!*engine_pending_fetches()) return 0;
    /* THE LIST IS COPIED BEFORE IT IS WALKED: engine_pending_fetches answers out of one buffer it reuses. */
    list = strdup(engine_pending_fetches());
    CHECK(list != NULL, "wpt: OOM copying the frontier's pending list");
    for (p = list; *p; ) {
        char *end = strchr(p, '\n');
        const char *method, *url;
        FetchRequest req;
        HeaderList none = { 0 };
        int i, rec = -1;

        if (!end) break;
        *end = 0;
        /* SPLIT WHERE IT WAS JOINED, by the engine's own splitter — three hosts each finding the TAB for
           themselves is three places for the grammar to drift. */
        engine_pending_split(p, &method, &url);
        /* AN ENTRY STAYS LISTED UNTIL IT IS ANSWERED, and this host now visits the list at every slice rather
           than once per answer — so without this the page's one `fetch` would become one REQUEST PER SLICE at
           the server, which is a different question asked repeatedly rather than the one the flow parked on. */
        if (wpt_request_asked(method, url)) { p = end + 1; continue; }
        for (i = 0; i < g_owed_n; i++)
            if (!strcmp(g_owed[i].url, url) && !strcmp(g_owed[i].method, method)) { rec = i; break; }
        req.method = method;
        req.url = url;
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
   named and already handed the page a WindowProxy for.
   `csp` AND `csp_self_origin` ARE ONE ARGUMENT IN TWO SLOTS — HTML §7.1.7 "Policy containers"' clone of the
   creator's container, whose CSP list CSP §2.2 "Policies" makes "a struct consisting of policies (a list of
   policies) and a self-origin". The child process is handed both because neither half is derivable there:
   §2.2.2 "Parse response's Content Security Policies" states a self-origin from OUTSIDE the policy bytes, and
   the only origin a child could state for itself is its own address's — which is the wrong one for an
   inherited list by construction (CSP §2.2's note: the self-origin exists for documents "that have inherited
   their policy").
   AND THE FOUR `coep*` SLOTS ARE THAT SAME CONTAINER'S §7.1.4 EMBEDDER POLICY ITEM, which crosses for exactly
   the reason the two above do: §7.1.7's clone-a-policy-container sets the clone's embedder policy to a COPY of
   the source's, so an item this process kept to itself is an inheritance the child would silently replace with
   `unsafe-none`. The two VALUES cross as §7.1.4's own tokens — a command line is a record, and a record
   carrying this enum's integers is a contract whose ends agree only by declaration order. */
static void wpt_spawn_child(const char *name, const char *url, const char *origin, const char *csp,
                            const char *csp_self_origin, const char *top_level_url,
                            const char *coep, const char *coep_endpoint,
                            const char *coep_report_only, const char *coep_report_only_endpoint)
{
    int down[2], up[2];
    pid_t pid;

    /* §7.1.4'S FOUR ITEMS ARE STATED BY EVERY CALLER, never defaulted here. The two callers ARE the two
       operations: a `navigable.create` carries the creator's, and §7.1.3.2's SWAP has no creator at all and
       so states §7.1.4's "a new embedder policy" in its own words. A `?:` here would make those one answer. */
    DCHECK(coep != NULL && coep_endpoint != NULL && coep_report_only != NULL &&
           coep_report_only_endpoint != NULL,
           "a child document was about to be provisioned with no §7.1.4 EMBEDDER POLICY for its §7.1.7 "
           "container — every container has one, so this is a caller that did not state which of the two "
           "answers applies (the creator's clone, or a new embedder policy where there is no creator)");
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
        /* APPENDED RATHER THAN INTERLEAVED, which is the rule this command line has followed since the
           self-origin was added to it: every position already read keeps its meaning, so the child's argv
           indexing has one rule and not one per generation. */
        execl(g_self_exe, g_self_exe, "--document", name, url, origin, csp ? csp : "",
              top_level_url ? top_level_url : "", csp_self_origin ? csp_self_origin : "",
              coep, coep_endpoint, coep_report_only, coep_report_only_endpoint, (char *)NULL);
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
static char *wpt_get_headers(const char *url, size_t *plen, HeaderList *pheaders);

/* Returns whether it ANSWERED anything, because the step loop has to know: it is one of the two things this
   host can do at a slice boundary, and a STALL it answers nothing to is a frontier waiting on a reply that is
   never coming rather than one waiting on this host (see the loop in main). The scheduler no longer re-enters
   a flow that is blocked on an unanswered request — it reports itself host-owed and leaves the pick — so the
   full-CPU spin this used to guard against is the scheduler's invariant now and not a rule of this file's. */
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
    /* NOT A CHILD, SO IT IS THIS PROCESS'S OWN DOCUMENT — the reply direction, which is most of what a real
       messaging protocol does: a frame answers its opener. The delivery is the same one a child makes, in this
       instance, so it goes through the same engine entry rather than a second path.
       AND IT IS ATTACHED, NEVER PERFORMED HERE. This host used to take the record apart itself and call
       window_message_route out of its own time: the task that lands is then enqueued against whichever flow
       the scheduler last had switched in, and the page's `message` listener lives in the delta of the flow
       that ran the script which registered it — so the message arrived at a document where nothing was
       listening, or at a timeline that never asked for it. engine_route makes it a work item of EVERY live
       timeline of the receiving document, each delivering under its own delta when the scheduler runs it,
       which is exactly what the child half already does with the same record — which is also why this takes
       the record and nothing out of it: the world vector, the target origin and the payload are fields of a
       grammar window_message.c and world.c own, and every one of them this host named was a copy of theirs. */
    DCHECK(!strcmp(doc, "wpt"),
           "a routed message named a document neither this process nor any child of it holds — the "
           "navigable.create notice for it was dropped, or the post outran it");
    engine_route(ctx, record, from_origin);
}

static void wpt_route_notice(JSContext *ctx, char *line, const char *from_origin)
{
    char *f[12]; int nf = 0; char *q;
    /* THE RECORD THE EMITTING ENGINE WROTE, kept whole before this router takes it apart. The split below
       writes NULs into `line`, and a routed post crosses to its instance VERBATIM — the receiving engine's own
       entry parses it, so a rejoin of the fields here would be this host restating a grammar it does not own. */
    char *whole = strdup(line);

    CHECK(whole != NULL, "wpt: OOM keeping a host notice whole");
    /* THE SPLIT STOPS AT THE LAST FIELD AND KEEPS THE REMAINDER VERBATIM, because the last field of a
       navigable.create IS a raw CSP header and HTTP allows HTAB inside one. */
    for (q = line, f[nf++] = q; *q && nf < 12; q++)
        if (*q == '\t') { *q = 0; f[nf++] = q + 1; }
    if (nf == 12 && !strcmp(f[0], "navigable.create")) {
        /* FIELD 5 IS HTML §8.1.3.1's TOP-LEVEL CREATION URL, FIELD 6 IS CSP §2.2's SELF-ORIGIN and FIELDS
           7-10 ARE §7.1.4's EMBEDDER POLICY — its value, its reporting endpoint, its report-only value and its
           report-only endpoint — all of which cross for one reason: they are ITEMS of HTML §7.1.7's clone of
           the creator's container, which moves whole, and the instance that will host the child can derive
           none of them. They all come BEFORE the policy at field 11 because the policy is the record's
           remainder: neither an origin's serialization nor an §7.1.4 value's token can contain a tab, and RFC
           8941 §3.3.3 "Strings" excludes one from the `report-to` endpoint, while a raw CSP header may hold
           one. */
        wpt_spawn_child(f[1], f[3], f[4], f[11], f[6], f[5], f[7], f[8], f[9], f[10]);
        free(whole);
        return;
    }
    /* `navigable.swap <new doc> <url> <origin>` — HTML §7.1.3.2's browsing context group SWAP, which is the
       SAME provisioning act and a different record because the two operations differ in every field the create
       carries beyond these three. There is no CREATOR (§7.3.2.3 creates the new browsing context "with null,
       null, and group"), so there is no policy container to clone and no opener policy to inherit; and the
       swapped-to navigable IS a top-level traversable, so HTML §8.1.3.1's top-level creation URL for its
       environments is its own address rather than something only the creator could state. Hence the empty
       policy and the address passed twice below — derived here, not sent twice and able to disagree.
       A NEW GROUP IS A NEW PROCESS HERE FOR THE SAME REASON A CROSS-ORIGIN CHILD IS: SECURITY.md keys an
       instance on `(browsing context group, origin)`, and a swap changes the first half. */
    if (nf == 4 && !strcmp(f[0], "navigable.swap")) {
        /* NO CREATOR MEANS NO CONTAINER TO CLONE, so both halves of the CSP list are empty rather than one of
           them being guessed at — an empty policy resolves no `'self'`, so there is no self-origin to state.
           §7.1.4's ITEM IS "A NEW EMBEDDER POLICY" FOR THE SAME REASON AND IS SPELLED OUT RATHER THAN LEFT
           EMPTY: the CSP list's absence is spelled by an absent self-origin, but an embedder policy has no
           absence — §7.1.7 gives every container one — so the swapped-to Document's is the initial value the
           section names, `unsafe-none` with two empty reporting endpoints. */
        wpt_spawn_child(f[1], f[2], f[3], "", "", f[2], "unsafe-none", "", "unsafe-none", "");
        free(whole);
        return;
    }
    /* `windowproxy.post <target doc> <world> <target origin> <base64>` — §9.4.4 across instances. THE ORIGIN
       IS STAMPED HERE, by the zone that knows who sent it: the engine may not name its own sender
       (SECURITY.md), so the notice carries none and this adds the one it knows. */
    if (nf == 5 && !strcmp(f[0], "windowproxy.post")) {
        wpt_route_post(ctx, f[1], whole, from_origin);
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
                        /* AND THE ANSWER NAMES THE CHILD'S TIMELINE, before the completion and separated from
                           it by the one TAB this line splits on: a world vector's own separators are ':' and
                           ',' (world_serialize), so the first tab is the boundary and the completion — which
                           may itself contain a tab — is the remainder. */
                        int completion = ENGINE_COMPLETION_NORMAL;
                        const char *sep = strchr(answer, '\t');
                        JSValue v;
                        char *world;
                        CHECK(sep != NULL && sep != answer,
                              "wpt: a child answered a cross-agent operation without naming the timeline that "
                              "computed it — a document's state is its flows, so an unnamed answer cannot be "
                              "told from another of that document's timelines answering the same question");
                        world = strndup(answer, (size_t)(sep - answer));
                        CHECK(world != NULL, "wpt: OOM reading the timeline that answered a cross-agent operation");
                        v = remote_completion_decode(ctx, sep + 1, &completion);
                        engine_host_answer(ctx, id, world, v, completion, ENGINE_ANSWER_PEER);
                        free(world);
                        answered = true;
                        JS_FreeValue(ctx, v);
                    }
                }
            }
            free(rec);
        }
        /* HTML §7.4.5 Populating a session history entry's CREATE NAVIGATION PARAMS BY FETCHING, which this
           runner answers itself because the document being loaded belongs to the instance that asked — routing
           it to a peer would be answering a same-origin load out of another agent. A body of null is a fetch
           that did not load, which is still a document the navigable gets.
           THIS CITATION WAS `§7.4 step 14` AND THAT IS NOT A CITATION ANYBODY CAN CHECK: §7.4 is the section
           "Navigation and session history", not an algorithm, so it has no step 14 — and the algorithm the
           number was reaching for, §7.3.1.3 Child navigables' create-a-new-child-navigable, has thirteen steps
           and does not fetch. The fetch is §7.4.5's, verified against the spec text rather than recalled. */
        else if (!strncmp(tab + 1, "document.fetch\t", 15)) {
            FetchRequest req;
            HeaderList none = { 0 };

            DCHECK(n < sizeof op, "a document address outgrew this runner's request buffer — truncating it "
                                  "would GET a different URL and skipping it parks the loading flow forever, "
                                  "so the buffer is what has to grow");
            memcpy(op, tab + 1, n); op[n] = 0;
            /* ASKED, NOT AWAITED — and this is the site where the difference is a DEADLOCK rather than a
               delay: what §7.4.5 is fetching is a document whose own instance this process has yet to fork, so
               a host that stood in the read could not spawn it, could not route to it, and could not run the
               flow that would. The answer is built at the completion (wpt_request_finish's DOCUMENT arm),
               which is where the response's header list and its bytes arrive together. */
            if (!wpt_request_asked_id(id)) {
                req.method = "GET";
                req.url = strchr(op, '\t') + 1;
                req.headers = &none;
                req.body = NULL;
                req.body_len = 0;
                wpt_request_begin(&req, WPT_DELIVER_DOCUMENT, id);
                answered = true;
            }
        }
        /* XHR §3.5.6's FETCH, which this runner answers with the same HTTP it answers every other request
           with. The record crosses as JSON because it carries a method, a header list and a body — a
           tab-separated line cannot hold a body — and because the answer already crosses that way.
           IT IS ANSWERED HERE AND NOT ROUTED TO A PEER: an XMLHttpRequest belongs to the instance that made
           it, and the trusted zone's job for one is the network, not the routing. */
        else if (!strncmp(tab + 1, "xhr.send\t", 9)) {
            char *line;
            JSValue rec, mv, uv, hv, bv;
            const char *method, *url, *bodys = NULL;
            size_t blen = 0;
            HeaderList hdrs = { 0 };
            FetchRequest req;
            uint32_t hn = 0, hi;

            /* THE REGISTER LISTS THIS UNTIL IT IS ANSWERED — see wpt_issue_pending's own guard. */
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
            req.headers = &hdrs;
            req.body = bodys;
            req.body_len = blen;
            /* ASKED, NOT AWAITED. `xml_http_request.c` calls this park "the one rendezvous in this engine that
               BLOCKS a flow" — and blocking the FLOW is the design; blocking the THREAD it shares with every
               other flow of the document was this host's own addition. The whole request is serialized into
               the record before this returns, so nothing it borrowed here has to outlive the call. */
            wpt_request_begin(&req, WPT_DELIVER_XHR, id);
            answered = true;
            header_list_free(&hdrs);
            if (method) JS_FreeCString(ctx, method);
            if (url) JS_FreeCString(ctx, url);
            if (bodys) JS_FreeCString(ctx, bodys);
            JS_FreeValue(ctx, mv); JS_FreeValue(ctx, uv); JS_FreeValue(ctx, hv); JS_FreeValue(ctx, bv);
            JS_FreeValue(ctx, rec);
        }
        /* AN OPERATION THIS HOST DOES NOT ROUTE IS A FLOW PARKED ON AN ANSWER NOBODY CAN SEND, and it CRASHES
           here rather than being left for the frontier to discover.
           It used to be left unanswered "because the asking flow stays parked, which is visible, where a wrong
           answer is not". The second half is right and the first half was false: what it was visible AS is a
           file that reports nothing and is killed at the gate's wall backstop ten minutes later, with the
           runner asleep and the op that caused it named nowhere. A missing route is a capability this host has
           not built — §Offensive programming's second kind — so it says which one, at the line that met it.
           Every op the engine raises today is routed above (windowproxy.get, object.*, document.fetch,
           xhr.send), so this cannot fire for anything that exists; it fires for the next one added. */
        else {
            DFAIL("a cross-agent operation reached this host under a verb it does not route — the flow that "
                  "asked is parked on an answer nothing in this process will ever produce. Route it beside "
                  "the four above, or state why this host is not the zone that answers it");
        }
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

/* A PAGE ERROR, AS IT HAPPENS — an uncaught throw from one of the programs the scheduler runs (solver/result.h).
   It used to be caught here, at the `while (JS_FlowResume)` that drove each program; with the programs on the
   frontier there is no such place, and a diagnostic this gate cannot print is a diagnostic it does not have.
   It is a report and NEVER a verdict: a browser's script that throws does not stop the document, so the flow
   runs on to the next program exactly as it would in Chrome, and testharness's own subtest results decide the
   file. */
static void wpt_page_error(const char *msg)
{
    fprintf(report_out(), "@WPTERR %s: %s\n", g_test_url[0] ? g_test_url : "<document>", msg);
}

/* THIS DOCUMENT'S PROGRAM SEQUENCE — what a session is opened over (engine.h). Each entry is its own program
   body and its own JS_FlowNew: classic scripts do not share a top-level scope, and concatenating them would
   leak per-<script> let/const bindings between two files the page kept apart. The scheduler BORROWS the arrays
   for the life of the session, so they outlive every statement that builds them and are freed with the run;
   `body` is taken (owned here from this point), `name` is copied, and `type` is HTML §4.12.1's script type —
   which of §8.1.4.4's two algorithms the scheduler runs this body through. A DOCUMENT test takes it from the
   <script> element it came from; everything this runner supplies itself (the prologue, the epilogue, the
   driver's META scripts and a `.any.js` test) is a CLASSIC script, which is a statement about those programs
   rather than a default.
   THE NAME AND THE ADDRESS ARE TWO COLUMNS, AND THIS RUNNER HAD ONE. `name` is what this host calls a program
   when it looks for one (wpt_insert_report finds the harness by it) and `address` is HTML §8.1.4.1 "Scripts"'s
   base URL — "the URL from which the script was obtained, for external scripts, or the document base URL of
   the containing document, for inline scripts" — which the engine resolves and hands to §8.1.4.4 as the
   script's base and, for a module, as its module map key. One column carried both, so EVERY program of every
   run was handed the engine as a row with source text AND an address, `<wpt-prologue>` included; the seed's
   invariant that a row is source-text-or-address (solver/engine.c) then aborted on the FIRST ROW OF EVERY
   TEST FILE IN THE CORPUS. A synthesized program has NO address — no bytes came from one — and saying
   otherwise would key a module map by a string that is not a URL. */
static char **g_prog_bodies, **g_prog_names, **g_prog_srcs;
static ScriptType *g_prog_types;
static int    g_prog_n, g_prog_cap;

static void wpt_program(char *body, const char *name, const char *address, ScriptType type)
{
    /* AN ADDRESS IS A URL OR IT IS ABSENT. This runner's own programs are named `<wpt-…>`, and that spelling
       reaching the address column is exactly the conflation this pair of columns exists to make impossible —
       the engine would resolve it as a `src`, hand it to §8.1.4.4 as the script's base URL, and for a module
       key the module map by it. */
    DCHECK(address == NULL || address[0] != '<',
           "a program synthesized by this runner was given an ADDRESS — no bytes came from one, so the name "
           "this host calls it by has been passed as the base URL §8.1.4.1 says a script is obtained from");
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

/* THE REPORT'S POSITION, ASKED OF THE PROGRAM SEQUENCE RATHER THAN OF THE MODE. It goes immediately after
   `/resources/testharness.js` — the slot WPT's own testharnessreport.js occupies — and that is ONE rule for a
   document (whose <script src> the corpus wrote) and for a `.any.js` (whose harness the driver passes as
   argv[1]). Two branches computing the same position is how the two come to disagree about it.
   A SEQUENCE WITH NO HARNESS IN IT IS REPORTED, NOT CRASHED: the two ways to reach that are a document whose
   `<script src>` 404'd — already printed above and already the driver's ABORT — and a file named on the command
   line that is not a testharness test at all. Neither is a missing capability, so neither is a DCHECK; what
   would be wrong is running the file with no report hook and letting it look like a file with no subtests. */
static void wpt_insert_report(void)
{
    static const char TAIL[] = "/resources/testharness.js";
    size_t m = sizeof TAIL - 1;
    int at = -1, k;

    /* ASKED OF THE NAME COLUMN, WHICH IS THE ONE EVERY PROGRAM HAS. The address column is NULL for a program
       no address supplied it, so a search there would silently skip the driver's own META scripts — and the
       `.any.js` harness this position is defined against is exactly one of those. */
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


/* ONE GET against the corpus server, resolved against the running document's address. */
/* `pheaders`, when given, receives the RESPONSE'S WHOLE HEADER LIST rather than one header out of it — the
   caller's to header_list_free. It used to hand back only the `Content-Security-Policy` string, and the rest of
   the list was parsed here and thrown away one line later: a corpus test whose `.headers` sidecar or
   `?pipe=header(...)` declares `Origin-Agent-Cluster`, `Cross-Origin-Opener-Policy` or
   `Cross-Origin-Embedder-Policy` was measured against a Document that never saw it. A header list is what
   §7.5.1 creates a Document from, so a header list is what leaves here. */
static char *wpt_get_headers(const char *url, size_t *plen, HeaderList *pheaders)
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
    if (pheaders) *pheaders = h;   /* MOVED, not copied — the caller frees it */
    else header_list_free(&h);
    if (body && (status < 200 || status >= 300)) { free(body); body = NULL; }
    return body;
}

static char *wpt_get(const char *url, size_t *plen)
{
    return wpt_get_headers(url, plen, NULL);
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
                           const char *top_level_url, bool requests_oac, OpenerPolicyValue opener_policy)
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
       PARAMETER, and fetch.c aborts on a fetch issued with none, which is what asserts this line is here.
       WHAT IT DOES WITH THE FLOW is the product's park (wpt_owe): the request goes on that flow's own pending
       register, and the two lines below are the other half of it — what this host still owes the frontier, and
       who reports a page error while the programs are the scheduler's rather than this file's. */
    { static const FetchProvider P = { wpt_owe }; fetch_set_provider(&P); }
    result_set_page_error_hook(wpt_page_error);
    /* …AND WHO EVALUATES A STRING HANDLER, which is the same kind of edge and was the one this host did not
       state. HTML §8.7's timer initialization steps compile a DOMString handler as a classic script when the
       timer fires, and core/timing/timer.c aborts on a string handler with no sink registered — so every
       `setTimeout("…")` in the corpus took its document down here while the two hosts that DO register it
       (main.c, test_forced.c) ran the same code correctly. An edge absent from one host is not a weaker host,
       it is an area this gate publishes a number for without having run it. */
    timer_set_script_sink(engine_queue_script);

    agent.origin = origin;
    agent.top_level_url = top_level_url;
    /* §7.5.1's requestsOAC, from the response the caller fetched — a WPT test document IS fetched from the
       corpus's own wptserver, so it has a response, and `html/browsers/origin/origin-keyed-agent-clusters/`
       delivers this header through `?pipe=header(...)` and `.headers` sidecars and nothing else. This host
       reads no header itself: the list goes through §7.4.6's navigation params like the product host's. */
    agent.requests_oac = requests_oac;
    /* §7.1.3's OPENER POLICY of the response that created this document — §7.3.2.3's group is created with it,
       and it comes through §7.4.6's navigation params like every other fact this host reads off a response. */
    agent.opener_policy = opener_policy;
    platform_agent_init(ctx, &agent);
}

/* ONE DOCUMENT. Runs once per document INCLUDING the first, which is what makes it the one description of what
   a document of this build is — a same-origin child navigable gets exactly this and nothing else. */
static void wpt_realm_install(JSContext *ctx, lxb_html_document_t *dom, const char *url, const char *origin,
                              SerializedPolicyContainer policy, SandboxFlags sandbox_flags,
                              uint32_t doc_id, JSValueConst nav_proxy)
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
    /* `wptTestComplete` — the vendor hook's other half, called from the completion callback WPT_REPORT
       registers. It is this runner's own like the two above, and like them it is never the browser's. */
    JS_SetPropertyStr(ctx, global, "wptTestComplete",
                      JS_NewCFunction(ctx, js_wpt_test_complete, "wptTestComplete", 0));

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
    doc.policy = policy;
    doc.sandbox_flags = sandbox_flags;
    doc.doc_id = doc_id;
    doc.nav_proxy = nav_proxy;
    platform_document_install(ctx, global, &doc);

    JS_FreeValue(ctx, global);
}

/* A SAME-ORIGIN CHILD NAVIGABLE'S REALM — a second JSContext in the SAME JSRuntime, which is what HTML's
   similar-origin window agent is. It gets the identical per-document install the first document got; there is
   no smaller variant of it, because a child whose `window` is smaller is a different browser. */
static JSContext *wpt_child_realm(JSRuntime *rt, lxb_html_document_t *dom, const char *url,
                                  const char *top_level_url, const char *origin,
                                  SerializedPolicyContainer policy, SandboxFlags sandbox_flags,
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
    wpt_realm_install(ctx, dom, url, origin, policy, sandbox_flags, doc_id, nav_proxy);
    /* THE CHILD'S SCRIPTS ARE THE CHILD'S, run in ITS realm — they are what make a popup a participant rather
       than an empty frame, since message-opener.html's whole body is one script that posts to its opener.
       THEY ARE QUEUED ONTO THE FRONTIER, NOT RUN HERE. A realm is built from inside §7.4 step 14's load job —
       a C activation in the middle of some flow's own program — so running the child's scripts at this point
       is a nested drive-to-completion re-entering JS from C: exactly the shape this engine refuses everywhere
       else, and it surfaced as `JS_ToPrimitiveFree reached with a real object` the moment a popup's script
       coerced anything.
       THEY GO ON THE RUNNING FLOW, NAMING THIS DOCUMENT, which is what the scheduler gained and what this host
       had a queue of its own for. A document's scripts are PROGRAMS, and a program is a work item of the ONE
       frontier: preemptible, parkable, ranked, and — because the queue entry names the DOCUMENT — compiled in
       the realm just built above rather than in the session's. They belong to the flow that CREATED the
       navigable because that is whose timeline this child exists in: a sibling arm that never navigated here
       has no such document, and a host-side queue could not have said so. */
    /* THE SEEDING IS THE DOCUMENT'S OWN AND NO LONGER THIS HOST'S. It used to be here, and here ONLY — of the
       three hosts that build a child realm, this one wrote the line and `main.c` and `test_forced.c` did not,
       so a same-origin child was inert in the shipped extension and alive in the corpus. That is the per-host
       drift `core/realm.h` exists to abolish, recurring through the one seam still copied per host, and it is
       why a same-origin iframe reported "the child is never analysed at all" on real Chrome while every WPT
       row that depends on a child's scripts passed. `navigable_seed_scripts` (core/frame/navigable.c) is that
       statement made once, for every host, and the copy that stood here queued each child program a SECOND
       time the moment it landed. */
    return ctx;
}

/* THE FIRST DOCUMENT, which is also what brings the agent up. A REAL LEXBOR PARSE — the same one the engine
   runs — of the test's own bytes when the test is an HTML file, and otherwise of the minimal document WPT's
   server wraps a `.window.js` in. One parse either way; the only difference is whose bytes. */
/* `inherited_csp` AND `inherited_csp_self_origin` ARE HTML §7.1.7 "Policy containers"' CLONE OF THE CREATOR'S
   CONTAINER, for a document this process did not root itself — which for this runner means a `--document`
   child, provisioned from another process's `navigable.create`. Both halves are empty for the top-level
   document, which has no creator. THEY WERE `(void)csp;` in wpt_child_main, which is the shape CLAUDE.md
   makes greppable: a field a producer writes, a consumer casts away, and nothing anywhere to say the child's
   inherited policy was never applied — every CSP question in a child instance answered "no policy". */
/* `html_headers` IS THE RESPONSE `html` IS THE BODY OF, and the two travel together or neither does — the same
   pairing core/frame/navigable.c's child_document asserts, for the same reason: a Document is created from a
   RESPONSE, and bytes with no headers is a load that never asked what it fetched while headers with no bytes
   are a response somebody else's. NULL for both is a caller with no response at all, which is HTML §7.4's
   initial about:blank and is an HTML document BY §7.4 rather than by anything defaulting here. */
/* `inherited_coep` AND ITS THREE COMPANIONS ARE THAT SAME CONTAINER'S §7.1.4 EMBEDDER POLICY ITEM. §7.1.7's
   clone-a-policy-container moves EVERY item of a container — "set clone's embedder policy to a copy of
   policyContainer's embedder policy" — so a clone that arrived without it would put a cross-origin-isolated
   creator's child under `unsafe-none`, with no header anywhere to say so because the child's own response
   never carried one. The values are §7.1.4's own TOKENS, which is what a `--document` child reads off its
   command line and what a `navigable.create` notice writes. */
static JSContext *wpt_build_document(const char *doc_name, const char *origin, const char *top_level_url,
                                     const char *html, size_t html_n, const HeaderList *html_headers,
                                     const char *inherited_csp, const char *inherited_csp_self_origin,
                                     const char *inherited_coep, const char *inherited_coep_endpoint,
                                     const char *inherited_coep_report_only,
                                     const char *inherited_coep_report_only_endpoint)
{
    static const char DOC[] = "<!doctype html><html><head></head><body></body></html>";
    char *fetched = NULL;
    HeaderList response_headers;
    /* THE RESPONSE THIS DOCUMENT IS CREATED FROM — the caller's when it fetched one, this function's own when
       it fetched the test file itself, and NULL when there is no response. It is one name rather than two
       branches because §7.4.5 reads a response and does not care which zone obtained it. */
    const HeaderList *response = NULL;
    /* §7.4.5's ARM for the document about to be built. DOC_LOAD_HTML with NO RESPONSE is not a default and not
       a guess: §7.4's initial about:blank — and an address whose fetch failed, for which a browser still shows
       a document — has no response to compute a type from and IS an HTML document by that section. Every case
       that HAS a response overwrites this from the response, below. */
    DocumentLoadType load = DOC_LOAD_HTML;
    NavigationParams np;
    const char *src = html;
    JSRuntime *rt;
    JSContext *ctx;

    DCHECK((html != NULL) == (html_headers != NULL),
           "the runner was asked to build a Document out of half a response — MIME Sniffing §7's computed type "
           "is a fact about the bytes those headers describe, so bytes with no header list is a document whose "
           "type was never asked and a header list with no bytes is one computed for another response");

    /* THE RESPONSE IS READ BEFORE THE AGENT EXISTS, and that ORDER is the spec's rather than this runner's
       convenience. §7.5.1 reads `Origin-Agent-Cluster` off the response and hands the answer to §8.1.2.2's
       obtain-a-similar-origin-window-agent, which is what ALLOCATES the agent — so a runner that brought the
       agent up first would have allocated the cluster before it knew which key the response asked for. The
       standard's own ordering says the same thing by calling the environment the read happens in RESERVED.
       THE BYTES COME WITH THE HEADERS. A corpus file is served by wptserve, whose `.headers` sidecars and
       `?pipe=header(...)` substitutions are the whole delivery mechanism for the areas this route exists for —
       `html/browsers/origin/origin-keyed-agent-clusters/` sends nothing else. */
    memset(&response_headers, 0, sizeof response_headers);
    if (src) {
        response = html_headers;
    } else {
        src = DOC;
        html_n = sizeof DOC - 1;
        if (g_html_mode) {
            fetched = wpt_get_headers(g_test_url, &html_n, &response_headers);
            CHECK(fetched != NULL, "wpt: the corpus server did not serve the HTML test file");
            src = fetched;
            response = &response_headers;
        }
    }
    /* ZERO IS THE ROOT NAVIGABLE'S TARGET SNAPSHOT SANDBOXING FLAGS: a top-level traversable has no embedder
       element, so §7.1.5 answers its creation flags from the POPUP sandboxing flag set, which begins empty and
       which only §7.3.1.7 "Navigable target names"'s rules for choosing a navigable ever fill — nothing
       chose this one. The other half of
       §7.4.5's union is this response's CSP-derived flags, which navigation_params computes.
       §8.1.3.5's SECURE-CONTEXT ANSWER is over the environment's TOP-LEVEL CREATION URL, which is what decides
       whether an `Origin-Agent-Cluster`, a COOP or a COEP is honoured at all. */
    /* THE PARAMS COME FROM THE RESPONSE THIS DOCUMENT IS CREATED FROM, whichever zone obtained it. They used to
       come from `response_headers` unconditionally, which is this function's OWN fetch — so a `--document`
       child, whose bytes and headers its caller had already fetched, was given the params of a response that
       does not exist: no `Origin-Agent-Cluster`, no COOP, no response CSP and no CSP-derived sandboxing flags,
       for a child whose whole delivery mechanism is a `?pipe=header(...)`. An EMPTY list is what a document
       with no response gets, and that is the right answer for that one: §7.4's initial about:blank carries no
       policy of its own and inherits its creator's, which is the clone applied below. */
    navigation_params_from_response(&np, response ? response : &response_headers, 0,
                                    secure_context_url_potentially_trustworthy(top_level_url));
    /* HTML §7.4.5's LOAD A DOCUMENT, ASKED BEFORE ANYTHING IS ALLOCATED — the same dispatch, from the same
       component, that core/frame/navigable.c's child_document runs for a child navigable, and this runner did
       not ask it at all: every top-level test document went to the HTML parser whatever the server served.
       THE COST WAS NOT AN ABSENT FEATURE, IT WAS A MISNAMED ONE. An `.xhtml`/`.xht`/`.xml` file is served by
       wptserve as `application/xhtml+xml` or `application/xml`, so MIME Sniffing §7 step 1 keeps that supplied
       type and §7.4.5 sends it to §7.5.3; under the HTML parser instead, a `<script>` is raw text (HTML
       §13.2.6.4.4 The "in head" insertion mode switches the tokenizer to §13.2.5.4 Script data state), so the
       XML §2.7 "CDATA Sections" opener `<![CDATA[` and XML §4.6 "Predefined Entities"' `&lt;` both reach the
       JavaScript compiler as program text. The document then failed as `a page <script> did not COMPILE` —
       true about the bytes, and pointing at the wrong subsystem for a document that was never JavaScript. The
       SAME files reached through an iframe already crashed by name, because that path goes through
       child_document: one absent capability reporting under two names depending on which loader arrived. */
    if (response) {
        MimeType computed;

        document_load_computed_type(&computed, response, src, html_n);
        load = document_load_type_of(&computed);
        mime_type_free(&computed);
        /* THE ARM THIS BUILD HAS NO LOADER FOR CRASHES BY NAME, one §7.5 subsection each — the statement
           child_document makes at its own site, and both are deleted together when that loader lands.
           §7.5.3's dependencies and the grammar still owed for it are enumerated there. */
        if (load != DOC_LOAD_HTML) DFAIL(document_load_type_section(load));
    }
    header_list_free(&response_headers);

    rt = JS_NewRuntime();
    JS_SetMaxStackSize(rt, 4 * 1024 * 1024);
    ctx = JS_NewContext(rt);
    wpt_agent_init(ctx, doc_name, origin, top_level_url, np.requests_oac, np.opener.value);
    /* §7.4 CALLS BACK HERE FOR A SAME-ORIGIN CHILD, because what a document of this build IS is this runner's
       answer and not the engine's — a child with a different platform surface would make every fidelity number
       measured in it a number about a different browser. */
    navigable_set_realm_builder(wpt_child_realm);

    g_wpt_dom = dom_document_create();
    CHECK(g_wpt_dom != NULL, "the runner's document allocation failed");
    /* THE HTML PARSER IS UNREACHABLE FOR A DOCUMENT §7.4.5 DOES NOT LOAD AS HTML, and this is the assertion
       that keeps it so. The dispatch above already crashes by name for every other arm, so this can only fire
       for a path added later that reaches this parse without going through it — which is exactly how this
       function came to hand XML to the HTML tokenizer in the first place. */
    DCHECK(load == DOC_LOAD_HTML,
           "the runner reached HTML §13.2's parser with a response HTML §7.4.5 loads as some other kind of "
           "document — the type dispatch is above and every arm but the HTML one crashes there, so this is a "
           "second route into the parse that never asked what it fetched");
    /* SHARED, for main.c's reason: this is the runner's ACTIVE document and every flow reads its tree. */
    CHECK(html_parse_document(g_wpt_dom, DOM_PARSE_ROOT_SHARED,
                              (const lxb_char_t *)src, html_n) == LXB_STATUS_OK,
          "the runner's document did not parse");
    free(fetched);

    /* THE ROOT NAVIGABLE IS THE HOST'S, so its §7.2.3 proxy is minted here — the same rule as every child,
       whose creator mints it. A navigable has one, and whoever owns the navigable is who makes it. */
    {
        /* THE RUNNER LOADED THIS DOCUMENT ITSELF, so it knows the navigable's name is the spec's initial "" —
           nothing opened it under a name. Saying so is what keeps `window.name` a computed value here. */
        /* §7.5.1's OPENER POLICY ROW for this root Document — §7.1.3's policy obtained from the same
           response the params above came from, so this runner and the product host give a root navigable the
           identical row from the identical computation. */
        JSValue root_proxy = window_proxy_new_self(ctx, world_local_doc(), "", np.opener.value);
        CHECK(!JS_IsException(root_proxy), "the root navigable's WindowProxy could not be allocated");
        /* §7.5.1's Document, from the navigation params the response decided — §7.4.5's final sandboxing flag
           set and §7.1.7 step 3's CSP list both come out of the one place a response is read
           (core/frame/navigation_params.c), so this runner and the product host create a Document from the
           identical computation over the identical input. */
        /* WHICH CSP LIST THIS DOCUMENT IS CREATED WITH, AND CSP §2.2's SELF-ORIGIN OF IT — HTML §7.1.7's own
           determine-navigation-params-policy-container, called rather than restated. §2.2.2 "Parse response's
           Content Security Policies" gives a response-delivered list "response's URL's origin", which for a
           document this runner FETCHED is `origin`; §7.1.7's CLONE keeps the origin it was cloned from, which
           for a `--document` child is its creator's and arrives on the command line. The self-origin is stated
           separately from `origin` beside it because they are two facts (a Document's principal, and the origin
           its policy resolves `'self'` against), and an inherited container is exactly what makes them
           disagree. THE ORDERING IS NOT THIS RUNNER'S TO HOLD: a copy of it here and another in main.c is two
           orderings, and the gate whose whole job is to measure the product host would be measuring its own. */
        EmbedderPolicyValue _coep = EMBEDDER_POLICY_UNSAFE_NONE, _coep_ro = EMBEDDER_POLICY_UNSAFE_NONE;
        SerializedPolicyContainer inherited;
        /* §7.1.4's THREE STRINGS, READ BACK — see embedder_policy.h for why they cross as tokens. A token
           naming none of them did not come off a header (that is §7.1.4.1's fail-open and belongs to `obtain`,
           which ran in the PARENT); it came off this runner's own command line, which its own parent wrote, so
           it is a record disagreeing with its writer and crashes rather than defaulting to `unsafe-none`. */
        SerializedPolicyContainer response;
        SerializedPolicyContainer policy;
        /* THE PARSE IS A STATEMENT AND THE ASSERT ONLY READS IT — a DCHECK condition is compiled out in
           release, so a parse performed inside one is a parse the shipped build never runs. */
        bool _ok = embedder_policy_value_of_token(inherited_coep, &_coep);
        bool _ok_ro = embedder_policy_value_of_token(inherited_coep_report_only, &_coep_ro);

        (void)_ok; (void)_ok_ro;
        DCHECK(_ok && _ok_ro,
               "a document was built with an §7.1.4 EMBEDDER POLICY VALUE naming none of the three strings the "
               "section defines — this runner writes these tokens onto a child's command line itself, so a "
               "token outside them is this program's two ends disagreeing about their own record");
        inherited = serialized_policy_container_or_none(
            inherited_csp, inherited_csp_self_origin,
            serialized_embedder_policy(_coep, inherited_coep_endpoint, _coep_ro,
                                       inherited_coep_report_only_endpoint));
        /* AND §7.1.4's ITEM OF THE RESPONSE'S OWN CONTAINER — §7.1.7's create-a-policy-container-from-a-fetch-
           response step 4, obtained where the response is read and handed to the constructor here. */
        response = serialized_policy_container(np.csp, origin,
                                               serialized_embedder_policy_of(&np.embedder));
        policy = policy_container_determine_navigation_params(g_base_url, response, inherited);

        wpt_realm_install(ctx, g_wpt_dom, g_base_url, origin, policy,
                          np.sandbox_flags, world_local_doc(), root_proxy);
        JS_FreeValue(ctx, root_proxy);
    }
    navigation_params_free(&np);   /* document_install built its container from it and keeps no pointer */
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
/* `<world><TAB><completion>` for the token in flight — the flow of the child that computed the answer and the
   answer itself — owned here until it is written. The world is on the line because a document's state is its
   flows: the parent has to be able to tell a SECOND timeline's answer from one answer relayed twice, and only
   the flow that produced each can say which. */
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
                /* THE REMAINDER IS `<world><TAB><completion>` AND IT CROSSES WHOLE. The world is the flow of
                   THIS instance that computed the answer, and the parent needs it to tell the peer's other
                   timelines from one answer delivered twice — so this channel relays it rather than reading it,
                   exactly as it relays the completion. Its presence is asserted at the parent's decode, where
                   the split happens and where a missing field would otherwise be read as part of the value. */
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
static void wpt_child_run(JSContext *ctx, const char *token)
{
    for (;;) {
        int r = engine_sched_step();
        int did;
        wpt_child_emit_notices(token);
        if (r == ENGINE_STEP_YIELD) continue;
        /* AND WHAT THIS HOST OWES, at the same boundary and for the same reason the top-level half pays it
           there: a child document fetches too (its own `<script src>` aside, its scripts call `fetch`), and a
           reply nobody supplies is a timeline parked for the life of the instance. A stall this fills is not a
           stall — the frontier has work again. */
        did = wpt_issue_pending();
        if (wpt_net_pump(ctx, /*block*/0) > 0) did = 1;
        if (did) continue;
        /* NOTHING RUNNABLE AND NOTHING FILLED. If an answer is on its way this instance waits for it — and it
           waits in the POLL rather than in a read, so the wait ends at whichever request lands first instead
           of at the one that happened to be issued first. With nothing in flight there is nothing to wait for
           and this is the stall the parent's next question resumes. */
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
    /* HTML §8.1.3.1's TOP-LEVEL CREATION URL, from the parent process that created this navigable. NOT
       defaulted to this document's own address: this instance's document may be NESTED, and answering from
       its own address would make an https child of an http page report itself a secure context — which is the
       ancestral hole Secure Contexts §4.2 exists to close, so a missing one is a parent that has not decided
       what it created rather than a value to guess. */
    const char *top_level_url = argc > 6 ? argv[6] : NULL;
    /* CSP §2.2's SELF-ORIGIN of the INHERITED list in `csp`, from the same parent for the same reason: §2.2.2
       states a self-origin from OUTSIDE the policy bytes ("response's URL's origin"), and the only origin this
       process could state for itself is its own address's — which for a clone of the CREATOR's container is
       the wrong one by construction (CSP §2.2's note names documents "that have inherited their policy"). It
       is LAST on the command line rather than beside `csp`, because appending is what keeps every position
       above it a fact this file already reads. */
    const char *csp_self_origin = argc > 7 ? argv[7] : "";
    /* §7.1.4's EMBEDDER POLICY of that same inherited container, its four items, from the same parent for the
       same reason: §7.1.7's clone moves a container WHOLE, so an item this process had to guess at would be an
       inheritance silently replaced by `unsafe-none` — and there is no header in this child's own response
       that could ever say otherwise, because the item belongs to the CREATOR's response and not to this one.
       APPENDED, like the self-origin above and for its reason: every position already read keeps its meaning.
       NOT DEFAULTED WHEN ABSENT — the CHECK below refuses a short command line instead. The two items above it
       could be spelled by an empty string because CSP §2.2's absence IS an empty pair; an embedder policy has
       no absence (§7.1.7 gives every container one), so "the parent did not state it" and "the parent stated
       `unsafe-none`" would be one value here, and that is the difference between a child a browser isolates
       and one it does not. */
    const char *coep = argc > 8 ? argv[8] : NULL;
    const char *coep_endpoint = argc > 9 ? argv[9] : NULL;
    const char *coep_report_only = argc > 10 ? argv[10] : NULL;
    const char *coep_report_only_endpoint = argc > 11 ? argv[11] : NULL;
    JSContext *ctx;
    char *html = NULL;
    size_t html_n = 0;
    /* THE RESPONSE'S HEADER LIST, WHICH TRAVELS WITH ITS BYTES. This child used to fetch with `wpt_get`, which
       throws the list away — so the Document it built was created from a response it could not read: HTML
       §7.4.5's computed type went unasked (every child parsed as HTML whatever the server served) and §7.5.1
       got no `Origin-Agent-Cluster`, no opener policy, no response CSP and no CSP-derived sandboxing flags,
       for an instance whose entire delivery mechanism in `html/browsers` is a `?pipe=header(...)`. */
    HeaderList html_headers;
    char line[65536];   /* a routed message rides this channel base64'd — see windowproxy.post below */
    /* THE SESSION'S PROGRAM SEQUENCE, which outlives every statement in this function: the scheduler BORROWS
       these arrays for the life of the session (engine_sched_begin), so a document's scripts may not be a
       block-scoped temporary the way they were when this host ran them itself. */
    DocScripts ds;

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
    CHECK(coep && coep_endpoint && coep_report_only && coep_report_only_endpoint,
          "wpt: a child document was started with no §7.1.4 EMBEDDER POLICY for its §7.1.7 policy container — "
          "the clone that created this Document moves every item of its creator's container, and this one is "
          "not derivable here: it belongs to the CREATOR's response, which this process never saw. Reading it "
          "as `unsafe-none` would put a cross-origin-isolated creator's child under the wrong rule for every "
          "no-CORS fetch it makes, with nothing anywhere to say so");
    snprintf(g_base_url, sizeof g_base_url, "%s", url && *url ? url : "about:blank");
    /* `about:blank` HAS NO BYTES TO FETCH — its Document is the empty one §7.4 creates, which is what makes it
       synchronous in a browser and what makes an <iframe> with no src scriptable immediately. */
    memset(&html_headers, 0, sizeof html_headers);
    if (strncmp(g_base_url, "about:", 6) != 0) {
        html = wpt_get_headers(g_base_url, &html_n, &html_headers);
        /* A child whose address does not load still HAS a document — a browser shows an error page and the
           navigable exists — so this is about:blank rather than a dead instance. THE LIST GOES WITH THE BYTES:
           a fetch that did not load is not a response, so its half-filled list is released here and the pair
           that reaches the build is (NULL, NULL). */
        if (!html) { html_n = 0; header_list_free(&html_headers); memset(&html_headers, 0, sizeof html_headers); }
    }
    /* THE CHILD PROCESS'S TOP-LEVEL CREATION URL came from the parent with the rest of the environment —
       this instance's document may be NESTED in a document of another instance, and only the zone that
       created it knows which. A cross-origin child that answered from its OWN address would report itself a
       secure context inside an insecurely-delivered page, which is exactly the ancestral hole Secure
       Contexts §4.2 exists to close. */
    ctx = wpt_build_document(name, origin, top_level_url, html, html_n, html ? &html_headers : NULL,
                             csp, csp_self_origin, coep, coep_endpoint, coep_report_only,
                             coep_report_only_endpoint);
    free(html);
    header_list_free(&html_headers);

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
    engine_sched_begin(ctx, ds.bodies, ds.srcs, ds.types, ds.els, ds.n, /*forking*/0, NULL);
    /* THE DOCUMENT RUNS BEFORE THE FIRST QUESTION ARRIVES, which is what makes a child a participant rather
       than an empty frame — message-opener.html's whole body is one script that posts to its opener, and that
       post has to be on this channel before the parent asks anything. */
    wpt_child_run(ctx, NULL);

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
            wpt_child_run(ctx, NULL);
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
    /* NO CREATOR: this process rooted its own top-level document from a response it fetched, so
       §7.1.7 has no container to clone and CSP §2.2.2's self-origin (this address's origin) is the
       right one. The empty pair is that statement rather than an absent argument.
       AND §7.1.4's ITEM IS "A NEW EMBEDDER POLICY" — the initial value §7.1.7 gives a container built without
       a creator, spelled out because an embedder policy has no ABSENCE to spell: the CSP list's absence is the
       empty self-origin beside it, and this item is always there. What this document's OWN response says about
       it is a different fact and is obtained from that response inside the build. */
    /* NO BYTES AND NO HEADERS FROM HERE: this call has no response of its own to hand over — wpt_build_document
       fetches the test document itself in `--test-document` mode, and a `.any.js` run has no document response
       at all (the skeleton it wraps the script in is this runner's, not a server's). */
    ctx = wpt_build_document("wpt", WPT_TOP_ORIGIN, g_base_url, NULL, 0, NULL, "", "",
                             "unsafe-none", "", "unsafe-none", "");
    /* THE REALM A RELAYED NOTICE IS DELIVERED INTO. A child's notice arrives while this process is blocked
       inside wpt_child_ask, which has no ctx of its own — this names the one it routes into, and a notice
       arriving before there is one is a message with nowhere to go. */
    g_wpt_ctx = ctx;
    rt = g_rt;

    /* THE PROGRAMS THIS RUN IS, built before the session because that is what a session is opened over. */
    {
        if (g_html_mode) {
            /* THE DOCUMENT'S OWN SCRIPTS, IN DOCUMENT ORDER — the same component the engine loads a real page
               with, so an HTML test's `<script src=/resources/testharness.js>` is loaded the way the page
               asked for it rather than by this runner guessing which files a document needs.
               NO PROLOGUE HERE. `GLOBAL` is the shim WPT's server writes around a `.any.js`, and a document
               that never asked for it must not be given one.
               AN EXTERNAL SCRIPT IS FETCHED INTO ITS OWN SLOT, in position, because classic scripts run in
               document order and one that did not load is a script that runs nothing rather than one that
               moves the others up. (The engine's own answer for an external script is the docscript park —
               the flow waits and the host fills the shared slot — and this host will use it the moment its
               network edge can carry a REQUEST rather than a URL; today's seam names only the URL, which
               cannot say which POST body a corpus handler is being asked to echo.) */
            DocScripts ds = document_exec_scripts(g_wpt_dom);
            for (i = 0; i < ds.n; i++) {
                /* AN INLINE `<script>` HAS NO ADDRESS — §4.12.1.1: "If el does not have a src content
                   attribute: Let base URL be el's node document's document base URL." The engine reads a NULL
                   address as exactly that, so naming the document here would be a second copy of a fact the
                   document already answers. */
                if (ds.bodies[i]) {
                    wpt_program(strdup(ds.bodies[i]), g_test_url, /*address*/NULL, ds.types[i]);
                    continue;
                }
                if (!ds.srcs[i]) continue;
                {
                    size_t len = 0;
                    char *body = wpt_get(ds.srcs[i], &len);
                    /* A <script src> THE SERVER DOES NOT HAVE is a real 404, and a real browser fires an
                       error event and carries on rather than stopping the document. Reporting it names the
                       file, because a test whose harness failed to load reports nothing at all otherwise. */
                    if (!body) {
                        fprintf(report_out(), "@WPTERR %s: <script src> did not load: %s\n",
                                g_test_url, ds.srcs[i]);
                        continue;
                    }
                    /* AND AN EXTERNAL ONE'S ADDRESS IS ITS OWN — §8.1.4.2 "Fetching scripts" creates the
                       script with "response's URL" as its base, and for a module that address is additionally
                       the module map key, so two `<script type=module src>` of one document are two modules.
                       The raw `src` attribute goes over as-is: §4.12.1.1's encoding-parse is relative to el's
                       node document, and the engine holds that document's realm. */
                    wpt_program(body, ds.srcs[i], ds.srcs[i], ds.types[i]);
                }
            }
            doc_scripts_free(&ds);
        } else {
            wpt_program(strdup(WPT_PROLOGUE), "<wpt-prologue>", /*address*/NULL, SCRIPT_TYPE_CLASSIC);
            /* THE HARNESS AND THE META SCRIPTS ARE PROGRAM INPUTS THE DRIVER RESOLVED, so they come from the
               paths it named; a `.sub.js` among them was fetched and substituted by the driver before it
               handed the path over. The TEST is not one of those — it is the run's ADDRESS — so it comes from
               the server. */
            for (i = 1; i < argc - 1 && !failed; i++) {
                size_t len = 0;
                char *src = read_file(argv[i], &len);
                if (!src) { fprintf(report_out(), "@WPTERR %s: cannot read\n", argv[i]); failed = 1; break; }
                /* NO ADDRESS: the driver handed this runner a PATH ON DISK and these bytes were read from it,
                   not obtained from a URL. §8.1.4.1's base URL is "the URL from which the script was
                   obtained", and there is none — so the document's own address answers, which is what a
                   generated `.any.html` gives a same-directory helper anyway. Naming a corpus URL these bytes
                   did not come from would be a fabricated address. */
                wpt_program(src, argv[i], /*address*/NULL, SCRIPT_TYPE_CLASSIC);
            }
            /* AND THE TEST ITSELF IS FETCHED, exactly as a document test is, because the reason is the same
               and it is not a reason about markup: a `.sub.` file is a TEMPLATE, and wptserve substitutes
               `{{host}}` and `{{ports[https][0]}}` when it serves one. Read off disk,
               `dom/events/EventListener-addEventListener.sub.window.js` opens an iframe at the literal string
               `https://{{hosts[alt][]}}:{{ports[https][0]}}/…`, which is not a URL — so the file tested the
               template rather than the test, and the driver's own substitution reached only the META scripts.
               One rule, both kinds: the test comes from the corpus's own server. */
            if (!failed) {
                size_t len = 0;
                char *src = wpt_get(g_test_url, &len);
                CHECK(src != NULL, "wpt: the corpus server did not serve the test file");
                /* AND THIS ONE DOES HAVE AN ADDRESS: these bytes came off the corpus server at g_test_url, so
                   that is §8.1.4.1's "URL from which the script was obtained" and what a relative
                   `import('./helper.js')` inside the test resolves against. */
                wpt_program(src, g_test_url, g_test_url, SCRIPT_TYPE_CLASSIC);
            }
        }
        /* THE REPORT AND THE EPILOGUE ARE PROGRAMS OF THIS DOCUMENT LIKE THE REST, and they are two rather than
           one: the report registers where WPT's own vendor hook goes (immediately after testharness.js, so no
           helper the test loads can be ahead of it), and the epilogue tells the harness the page is done, which
           can only be the last thing that runs. */
        if (!failed) {
            wpt_insert_report();
            wpt_program(strdup(WPT_EPILOGUE), "<wpt-epilogue>", /*address*/NULL, SCRIPT_TYPE_CLASSIC);
        }
    }

    /* THE ONE SCHEDULER, over this document's programs. What stood here was a fabricated flow — one member
       added at agent init and left switched in for the whole run so that everything asking flow_running() had
       an answer — and a pump beside it: a `while (JS_FlowResume)` per program, a `while (JS_ResumeParkedFlow)`,
       a `JS_ExecutePendingJob` drain, and this host's own copy of the event loop's order (the document
       lifecycle, then the rendering opportunity, then the due timer). Every one of those is the scheduler's:
       the flow's jobs are ITS queue, the parked continuation is resumed by flow_step before anything else the
       flow could do, and the three task sources are asked through the hooks their components registered. A
       host that kept its own copy was a second scheduler with the frontier bypassed, which is what CLAUDE.md
       §scheduler forbids at every depth — and it measured a model the product does not have.
       FORKING IS OFF: this gate measures the browser half against a spec oracle, so a concolic branch must not
       explore both arms — the same choice the child half makes, and the same one the @S candidate re-fire
       makes for its own reason. */
    /* NO ELEMENT COLUMN, AND THAT IS A STATEMENT ABOUT THIS LIST. These programs are the harness's own —
       a prologue, testharness.js, the report hook, an epilogue — flattened by wpt_program out of a
       document rather than collected as its `<script>` elements, so no row of it came from one. §3.1.7's
       `currentScript` is null while each of them runs, which is that section's answer for a document that
       is not executing a script element. The OTHER entry above passes document_exec_scripts's column. */
    engine_sched_begin(ctx, g_prog_bodies, g_prog_srcs, g_prog_types, /*els*/NULL, g_prog_n,
                       /*forking*/0, NULL);
    for (;;) {
        int r = engine_sched_step();
        int did;

        /* THE MEASUREMENT ENDED, so the session does — HTML §7.3.1.6 "Navigable destruction". `done()` is
           testharness's own end of the test and the verdict is already printed (js_wpt_test_complete says why
           this is the end of a MEASUREMENT and not a bound on work). What performs the destruction is the
           teardown below this loop: every document this process holds is released, and each child navigable is
           a PROCESS whose pipes are closed and which is reaped — which is the shape of that section's
           destroy-a-top-level-traversable, "for each historyEntry ... destroy a document and its descendants"
           and then the traversable itself. The requests still in flight go with it, as the loads of a
           destroyed navigable do; wpt_net_free says so from the other end. */
        if (g_test_complete) break;

        /* THE HOST'S HALF OF EVERY SLICE BOUNDARY, and it is asked at EVERY one rather than only at a stall —
           the same protocol the extension's bridge speaks (engine.h): paying only at a stall makes one flow's
           reply conditional on every other flow in the document also becoming blocked. Notices first: a
           document announced but not yet provisioned is one a read would arrive for before its instance
           exists. */
        did = wpt_answer_host_requests(ctx);
        if (wpt_issue_pending() > 0) did = 1;
        if (wpt_net_pump(ctx, /*block*/0) > 0) did = 1;
        if (r == ENGINE_STEP_DONE) break;
        if (r == ENGINE_STEP_STALLED && !did) {
            /* NOTHING RUNNABLE, AND THIS HOST FILLED NOTHING — which is a WAIT if an answer is on its way and
               a DISAGREEMENT if one is not, and those were the same line until the network edge could tell
               them apart. A request in flight means the frontier is parked on the network, which is the
               ordinary state of a browser: this host has no work of its own left, so it sleeps in the poll
               until one of the answers lands and then re-ranks. NO TIMEOUT — §NO BOUNDS: a reply that never
               comes leaves its flow parked, and a deadline could only truncate one that was merely slow.
               THE TWO ANSWERS ABOUT ONE QUESTION MUST AGREE. With nothing in flight, the scheduler says it is
               owed something and this host says it owes nothing, so the flows parked here are waiting for a
               reply nobody is going to send. In release there is nothing to fix it with, so the run ends with
               the flows parked and the teardown names what was dropped. */
            DCHECK(g_inflight_n > 0,
                   "the frontier stalled on work this host says it owes, this host filled NOTHING, and it has "
                   "NOTHING IN FLIGHT — the stall hook and the network edge are two answers to one question "
                   "and they disagree, so the flows parked here are waiting for a reply nobody will send");
            if (!g_inflight_n) break;
            wpt_net_pump(ctx, /*block*/1);
        }
    }
    /* AND THE SESSION ENDS HERE, UNCONDITIONALLY, because this loop has THREE exits and only one of them ends
       a session by itself. DONE drained the frontier and closed it; the other two are this host deciding to
       stop — the measurement being over, and a stall this host cannot pay — and each of those leaves a flow
       SWITCHED IN, with its heap delta applied to the shared baseline, its created nodes in the document and
       its decision state in the scheduler's globals rather than its own blob. The teardown below then releases
       one copy of each while the scheduler still holds the other, which flow_release asserts and which is what
       26 files in `html/browsers` aborted on the first time this loop grew a second exit.
       ENDING IS THE ORDINARY SUSPEND, so this drops nothing: every member of the frontier is left a SNAPSHOT,
       which is the state §Time-travel says a parked flow is in and the state §7.3.1.6's destruction needs them
       in before it can be a mechanism rather than a free. The flows are not finished and this does not pretend
       they are — they are suspended, and nothing resumes them because a gate runner is one document in one
       process with no frontier that outlives it.
       IT CARRIES NO `if`, and that is the correction rather than an economy: which exits have already closed
       the session is a question the ENGINE answers (engine.h), and a copy of that answer in each host is a
       condition every new exit has to remember to be included in. */
    engine_sched_end();

    /* THE PROGRAM SEQUENCE AND THE REQUEST RECORDS, both of which outlived the session by design and neither
       of which anything else owns. The session BORROWED the arrays (engine.h), so they are freed only now that
       it has ended; a record still here is a request whose flow was parked when the run stopped, which the
       frontier's own teardown asserts about from the other end. */
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

    /* THE PLATFORM'S OWN LIST, UNDONE — see main.c's teardown: one call, whatever this browser declared. */
    platform_agent_free();
    /* rendering, timer, message_port and event_target are ROWS on core/platform.h's release column now,
       run by the platform_agent_free above. Each of them CLAIMED a slot in another component — §8.1.7's
       timer step and §8.1.7.3's in-parallel half on the ONE frontier, HTML §8.1.7.2's handler-set hook and
       §2.9's tree walk and activation behaviour in the events layer — and not one of those claims was ever
       given back. Two of these three hosts also ran event_target_free BEFORE message_port_free, which is
       the order of that pair exactly backwards; reverse declaration order is what decides it now. */
    page_reveal_free(ctx);
    media_query_list_free(ctx);
    viewport_free();
    visual_viewport_free();
    /* §7.2.4's Location is a ROW on core/platform.h's release column now, run by the platform_agent_free
       above. It holds TWO CLAIMS in solver/concolic.c's source registry, and concolic_free asserts that
       registry is empty — an ordering that used to rest on all three hosts writing these two lines the same
       way round, and that reverse declaration order decides now. */
    session_history_free();
    history_free();
    navigation_free(ctx);              /* HTML §7.2.6 the navigation API */
    navigation_history_entry_free(ctx);
    animation_frame_free(ctx);
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
    navigable_free(ctx);
    /* NOTHING SWITCHES THE RUNNING FLOW OUT HERE ANY MORE, and the line that did is deleted rather than kept
       as a safety net: a session ends by CLOSING (engine_session_close performs exactly that switch-out, which
       is what makes the frontier a set of snapshots), so a host doing it as well would be the second scheduler
       this conversion removed, expressed as one line of teardown. A flow still switched in at this point is a
       session that never closed, and flow_release says so at the flow it reaches — which is what the
       `engine_sched_end` above the teardown is for, and it is the ENGINE's close rather than a switch-out this
       file performs, so the two lines say the same thing from the two sides rather than contradicting. */
    /* THE SOLVER'S OWN LIST, UNDONE — one call, in solver/engine.h, for the reason the platform's is one call:
       these six lines were hand-copied into three hosts and had already drifted three ways. See that header. */
    solver_agent_free(ctx);
    document_free(ctx);
    /* THE WHOLE DOM GROUP — element_free's cascade, the <iframe> element and GEOMETRY INTERFACES §3/§4 — is a
       set of ROWS on core/platform.h's release column now, run by the platform_agent_free above. This LINE
       stays: document_free releases a REALM's record, not the agent's. The component's other half —
       document_agent_free — is a row on that column, and the two are not ordered against each other. See
       main.c's teardown. */
    window_free(ctx);
    remote_object_free(ctx);
    window_proxy_free(ctx);
    report_exception_free(ctx);
    event_free(ctx);
    realm_intrinsics_free();   /* the DECLARATIONS are the agent's; each realm's prototypes went with it */
    queuing_strategy_free(ctx);
    readable_stream_free(ctx);
    blob_free(ctx);
    /* The File System model and its two standards, the two delivery callees, §9.5's bus and
       XMLHttpRequest are ROWS on core/platform.h's release column now — this runner never had the
       XMLHttpRequest line at all, so §5's ProgressEvent slot Symbol leaked in every file it ran. */
    encoding_free(ctx);
    text_stream_free(ctx);
    /* AFTER THE FRONTIER (solver_agent_free above), because a flow parked inside an IDL member reads this pool
       at its teardown — the position this runner already had, and the one idl_args_free now asserts. */
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
       unfinished machine by the step it rests at. The IDL pool's BLOCKS are the same obligation: each holds
       the definition the runtime borrowed. */
    idl_args_pool_free();
    idl_async_iter_free();
    return failed ? 1 : 0;
}
