/* THE PRODUCTION ABI ENTRY — the `qjs_*` exports `extension/bridge.js` drives, and nothing else.
 *
 * The OLD main.c was deleted with the rest of the pre-rewrite host because it was written against the fork's
 * previous hook API. This one is written against the current one (JS_SetTimeTravelHooks + JS_FlowNew/Resume +
 * engine_sched_begin/step) and holds NO semantics: identity is browser/core/loader's Lexbor <script> scan, the
 * frontier is solver/engine.c's WFQ dispatch loop, and both were extracted into components so this layer could
 * be exactly this thin.
 *
 * The ABI is begin+step rather than one run because of the cooperative-quantum yield: the host has a message
 * port to pump, other documents' engines to interleave and findings to stream, so the scheduler RETURNS on a
 * bounded slice and resumes the byte-identical frontier. engine_sched_step already speaks that protocol.
 *
 * The WEB-PLATFORM host edges (fetch/DOM/location relayed to the trusted bridge) are NOT installed here yet, and
 * they are honestly ABSENT rather than stubbed: a page reading a global this engine does not provide throws its
 * own TypeError, which is the forcing function that names the edge to build. A stub answering undefined would
 * let a flow run past the missing capability and report a surface it never actually reached. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <lexbor/html/html.h>

#include "check.h"
#include "quickjs.h"
#include "browser/core/fetch/fetch.h"
#include "browser/core/dom/document.h"
#include "browser/core/dom/element.h"
#include "browser/core/frame/location.h"
#include "browser/core/loader/document_scripts.h"
#include "browser/core/loader/module_loader.h"
#include "solver/absent.h"
#include "solver/concolic.h"
#include "solver/cow.h"
#include "solver/endpoint.h"
#include "solver/engine.h"
#include "solver/flow.h"
#include "solver/result.h"
#include "solver/solve.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#define QJS_EXPORT EMSCRIPTEN_KEEPALIVE
#else
#define QJS_EXPORT
#endif

/* One WASM instance per DOCUMENT (SECURITY.md), so the instance IS the document: one parse, one script
 * inventory, one engine. Nothing here is a registry — a second document is a second instance. */
static lxb_html_document_t *g_dom;
static DocScripts           g_scripts;
static unsigned             g_bundle_id;
static JSRuntime           *g_rt;
static JSContext           *g_ctx;
static int                  g_begun;
static int                  g_done;

/* PHASE 1 — parse and identify. No script runs, which is the whole point: the host reads the frontier key back
 * synchronously and looks up the prior session before any flow is seeded. */
QJS_EXPORT int qjs_init(const char *code, const char *html,
                        const char *origin, const char *unused, const char *csp)
{
    CHECK(g_dom == NULL, "qjs_init ran twice in one WASM instance — one instance is one document");

    g_rt = JS_NewRuntime();
    CHECK(g_rt != NULL, "the runtime allocation failed: a dropped engine loses the whole frontier");
    JS_SetMaxStackSize(g_rt, 4 * 1024 * 1024);   /* align quickjs's overflow check with the 8MB wasm stack */
    g_ctx = JS_NewContext(g_rt);
    CHECK(g_ctx != NULL, "the context allocation failed");

    concolic_init(g_ctx);
    flow_registry_init();
    endpoint_init();
    solve_init(g_ctx);

    /* Per-flow isolation: the time-travel RECORD boundary. Installed AFTER the context's own globals exist, so
       the baseline is pre-flow and nothing set up here lands in a delta. */
    static const JSTimeTravelHooks TIME_TRAVEL = {
        .prop_write = cow_capture_hook, .cell_write = cow_capture_varref,
        .arr_append = cow_capture_arr_append, .gen_fork = engine_gen_fork,
        .map_add = cow_capture_map_add, .map_mutate = cow_capture_map_mutate,
        .async_state = cow_capture_async_state, .module_eval = cow_capture_module_eval, .async_fork = cow_capture_async_fork };
    JS_SetTimeTravelHooks(&TIME_TRAVEL);
    /* Concolic VALUE propagation stays installed across scheduling AND verification — taint must flow during a
       candidate re-fire too. The exploration hooks (branch/fork/preempt) are owned and scoped by the scheduler. */
    static const JSConcolicHooks CONCOLIC = {
        .add = concolic_add_hook, .cmp = concolic_cmp_hook, .is = concolic_is,
        .absent = absent_global_hook, .rel = concolic_rel_hook, .type_of = concolic_typeof_hook };
    JS_SetConcolicHooks(&CONCOLIC);

    g_dom = lxb_html_document_create();
    CHECK(g_dom != NULL, "the document allocation failed");
    if (lxb_html_document_parse(g_dom, (const lxb_char_t *)(html ? html : ""),
                                html ? strlen(html) : 0) != LXB_STATUS_OK)
        CHECK_FAIL("the document parse failed — the DOM is the ground truth every flow reads");

    /* Identity and script inventory from the DOM's OWN executable scripts, never from `code`: a concatenation
       cannot represent per-<script> scope and would shift with an inline script the page did not ship. */
    g_bundle_id = document_bundle_id(g_dom);
    g_scripts   = document_exec_scripts(g_dom);

    /* The web-platform surface, installed on the BASELINE — before any flow runs, so these globals are
       pre-flow state and never land in a delta. Each is a real component under browser/; what is not built
       yet is absent, and the page's own throw on reading it names the next one to write. */
    {
        JSValue g = JS_GetGlobalObject(g_ctx);
        /* WHAT THE PLATFORM OWNS, declared whether or not it is built yet. A name on this list is the
           ENGINE's to provide, so an unbuilt one stays absent and the page's ReferenceError names the
           component to write; everything else the page reads and nothing defines is the server's, and comes
           back symbolic so the gate behind it forks. Without this every missing Web API would be mistaken for
           app state and a flow would run past it reporting a surface it never reached. */
        static const char *const PLATFORM[] = {
            "fetch", "location", "document", "window", "navigator", "localStorage", "sessionStorage",
            "history", "screen", "XMLHttpRequest", "WebSocket", "postMessage", "addEventListener",
            "requestAnimationFrame", "IntersectionObserver", "MutationObserver", "getComputedStyle",
            "customElements", "indexedDB", "crypto", "caches", "performance",
        };
        for (size_t pi = 0; pi < sizeof(PLATFORM) / sizeof(PLATFORM[0]); pi++)
            absent_declare_platform(g_ctx, PLATFORM[pi]);

        /* `window` IS the global object (7.2.2: the Window object's [[Get]] is the global's), so a bundle
           reading window.X and one reading X are the same read spelled two ways. It was on the platform list
           and never installed, which made every `window.__FLAGS` in a real bundle a ReferenceError. */
        JS_SetPropertyStr(g_ctx, g, "window", JS_DupValue(g_ctx, g));
        JS_SetPropertyStr(g_ctx, g, "self",   JS_DupValue(g_ctx, g));
        fetch_install(g_ctx, g);
        module_loader_install(g_rt);
        location_install(g_ctx, g, origin);
        element_init(g_ctx);
        document_install(g_ctx, g, g_dom, origin);
        JS_FreeValue(g_ctx, g);
    }

    (void)code; (void)unused; (void)csp;
    return 0;
}

/* The frontier KEY's document half. Pure scan, so the host may ask the instant qjs_init returns. */
QJS_EXPORT unsigned qjs_bundle_id(void)
{
    DCHECK(g_dom != NULL, "qjs_bundle_id was asked before the document was parsed");
    return g_bundle_id;
}

/* What the host still owes the frontier: the replies parked flows are waiting on. The scheduler asks this
   before it decides the frontier is exhausted. */
static int qjs_owed(void)
{
    return *engine_pending_urls() != '\0';
}

/* PHASE 2 — seed the frontier. */
QJS_EXPORT void qjs_begin(const char *recipes)
{
    DCHECK(g_ctx != NULL, "qjs_begin ran before qjs_init built the context");
    DCHECK(!g_begun, "qjs_begin ran twice — the frontier is seeded once and stepped thereafter");
    if (recipes && *recipes)
        DFAIL("qjs_begin was handed parked recipes — build the cold-tier resume that rebuilds each suspended "
              "flow's snapshot from its recipe and re-ranks it into the one frontier");
    engine_set_stall_hook(qjs_owed);
    engine_sched_begin(g_ctx, g_scripts.bodies, g_scripts.n, /*forking*/1);
    g_begun = 1;
}

/* One cooperative quantum of the ONE dispatch loop. ENGINE_STEP_YIELD leaves every flow where it was;
 * ENGINE_STEP_DONE means the frontier is empty. */
QJS_EXPORT int qjs_step(void)
{
    int r;

    DCHECK(g_begun, "qjs_step ran before the frontier was seeded");
    if (g_done)
        return ENGINE_STEP_DONE;   /* the session is over; stepping it again is not a question to ask twice */
    r = engine_sched_step();
    if (r == ENGINE_STEP_STALLED)
        return ENGINE_STEP_YIELD;   /* the bridge speaks two values; a stall is "call me again", same as a slice */
    g_done = (r == ENGINE_STEP_DONE);
    return r;
}

/* The ONE result document, serialized directly from the C findings — the host does one JSON.parse of it. */
QJS_EXPORT const char *qjs_result(void)
{
    DCHECK(g_begun, "qjs_result was asked of an engine that never ran");
    return result_json();
}

QJS_EXPORT void qjs_teardown(void)
{
    DCHECK(g_dom != NULL, "qjs_teardown ran on an instance that was never initialised");
    /* A flow waiting on a reply is not a finished one: it holds a snapshot, a COW delta and a parked
       continuation. flow_finish asserts that a flow may not finish with a continuation parked; tearing the
       instance down goes around that assert and frees the whole frontier, so the same rule is stated where it
       would otherwise be evaded. The host owes what qjs_pending told it. */
    DCHECK(*engine_pending_urls() == '\0',
           "qjs_teardown with replies still owed — every flow parked on one is dropped with its continuation. "
           "Provide them, or step to DONE, before ending the session");
    doc_scripts_free(&g_scripts);
    if (g_ctx) { JS_FreeContext(g_ctx); g_ctx = NULL; }
    if (g_rt)  { JS_FreeRuntime(g_rt);  g_rt  = NULL; }
    lxb_html_document_destroy(g_dom);
    g_dom = NULL;
    g_begun = 0;
    g_done = 0;
}

/* ---- The capabilities this entry cannot reach yet -----------------------------------------------------
 * Each aborts at its own entry naming what to build. Answering "" or 0 instead would have the host believe an
 * engine that reports no pending fetches while a flow is parked on one that never arrives — the protocol would
 * run to completion over the hole and the result would look finished. */

QJS_EXPORT const char *qjs_pending(void)
{
    DCHECK(g_begun, "qjs_pending was asked of an engine that never ran");
    return engine_pending_urls();
}

/* The lazily-loaded SCRIPT URLs — the headline moat surface, and a separate list from qjs_pending only because
   the host fetches them differently (a JS body is executed, a data body is handed back). The script-load edge
   that fills this is core/loader's, and it is the next component: until it exists a page's lazy chunk arrives
   through fetch like any other URL, which is honest but misses `import()` and an injected <script>. */
QJS_EXPORT const char *qjs_chunks(void)
{
    DCHECK(g_begun, "qjs_chunks was asked of an engine that never ran");
    return module_loader_chunks();
}

QJS_EXPORT void qjs_provide(const char *url, const char *body)
{
    DCHECK(g_begun, "a body was provided to an engine that never ran");
    /* ONE delivery for every parked request. A fetch's reply and a lazy CHUNK's source both settle a promise the
       flow registered before it suspended — the chunk's is the module loader's source promise, so the load
       finishes on its reaction and the page's `await import(...)` continues. Neither is the host's to
       distinguish, and neither re-runs anything. */
    {
        JSValue v = body ? JS_NewString(g_ctx, body) : JS_UNDEFINED;
        int n = engine_provide(g_ctx, url, v);
        JS_FreeValue(g_ctx, v);
        if (n == 0)
            DFAIL("a body was provided for a URL no flow is parked on — the host's pending/provide pairing is "
                  "off, and resolving nothing would leave the flow that IS parked waiting forever");
    }
}

QJS_EXPORT double qjs_top_weight(void)
{
    DCHECK(g_begun, "qjs_top_weight was asked of an engine whose frontier was never seeded");
    return engine_top_weight();
}

QJS_EXPORT void qjs_set_yield_floor(double floor)
{
    engine_set_yield_floor(floor);
}

QJS_EXPORT void qjs_request_park(void)
{
    DFAIL("qjs_request_park — build the cold-tier park: serialize the lowest-value suspended tail to recipes so "
          "the host can write them to IndexedDB and resume them in a later session");
}

/* STREAM what is known so far. The host reads findings off the print sink, so this writes the same one result
   document qjs_result returns, on the same @RESULT line the smoke entry uses — a long analysis reports as it
   goes instead of only at the end. It READS: no flow is touched, nothing is drained, and the frontier the next
   step resumes is the one this was called on. */
QJS_EXPORT void qjs_emit_partial(void)
{
    DCHECK(g_begun, "qjs_emit_partial was asked of an engine that never ran");
    printf("@RESULT %s\n", result_json());
    fflush(stdout);
}
