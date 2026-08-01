/* THE WASM ENTRY — the untrusted bundle's `qjs_*` ABI, and nothing else.
 *
 * SECURITY.md puts every capability the sandbox lacks behind the trusted JS bridge: it cannot fetch, cannot
 * touch IndexedDB, cannot paint. So this file is the thirteen entry points `extension/bridge.js` calls and the
 * plumbing to reach the components that already exist — it holds NO semantics of its own. Document identity is
 * a Lexbor <script> scan (browser/core/loader), the frontier is the WFQ dispatch loop (solver/engine.c), and
 * both were extracted out of the scheduler monolith precisely so this layer could be thin.
 *
 * The cooperative-quantum yield is why the ABI is `begin` + repeated `step` rather than one `run`: the host has
 * a message port to pump, other documents' engines to interleave and findings to stream, so the scheduler
 * RETURNS on a bounded wall-clock slice and resumes the byte-identical frontier. engine_sched_step already
 * speaks that protocol; qjs_step is its export.
 *
 * WHAT IS NOT BUILT YET CRASHES HERE, at the entry that names it. A noop would let the bridge's protocol run to
 * completion against an engine that silently learned nothing — a lazy chunk never fetched, a parked flow never
 * written — and the suite would stay green over the hole. */
#include <stdlib.h>
#include <string.h>
#include <lexbor/html/html.h>

#include "check.h"
#include "quickjs.h"
#include "browser/core/loader/document_scripts.h"
#include "solver/engine.h"

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

/* PHASE 1. Parse the document and compute its identity — no script runs, which is the whole point: the host
 * reads the frontier key back synchronously and looks up the prior session before any flow is seeded.
 * `code` is the concatenated script text the content script captured, kept for the callers that still hand it
 * over separately from the DOM; `html` is the document. */
QJS_EXPORT int qjs_init(const char *code, const char *html,
                        const char *origin, const char *unused, const char *csp)
{
    size_t html_len = html ? strlen(html) : 0;

    CHECK(g_dom == NULL, "qjs_init ran twice in one WASM instance — one instance is one document");
    g_dom = lxb_html_document_create();
    CHECK(g_dom != NULL, "the document allocation failed: a dropped document loses the whole frontier");
    if (lxb_html_document_parse(g_dom, (const lxb_char_t *)(html ? html : ""), html_len) != LXB_STATUS_OK)
        CHECK_FAIL("the document parse failed — the DOM is the ground truth every flow reads");

    /* Identity from the DOM's OWN executable scripts, never from `code`: the concatenation cannot represent
       per-<script> scope and would change with an injected inline script the page did not ship. */
    g_bundle_id = document_bundle_id(g_dom);
    g_scripts   = document_exec_scripts(g_dom);

    g_rt = JS_NewRuntime();
    CHECK(g_rt != NULL, "the runtime allocation failed");
    g_ctx = JS_NewContext(g_rt);
    CHECK(g_ctx != NULL, "the context allocation failed");

    (void)code; (void)origin; (void)unused; (void)csp;
    DFAIL("qjs_init reached the host-edge install with a parsed document — build the document/origin/CSP "
          "environment install (location, document, safeFetch relay, policy from the response header) before "
          "the first flow is seeded");
    return 0;
}

/* The frontier KEY's document half. Pure scan, so the host may call it the instant qjs_init returns. */
QJS_EXPORT unsigned qjs_bundle_id(void)
{
    DCHECK(g_dom != NULL, "qjs_bundle_id was asked before the document was parsed");
    return g_bundle_id;
}

/* PHASE 2. Seed the frontier — fresh, or resumed from the parked recipes the host read out of IndexedDB. */
QJS_EXPORT void qjs_begin(const char *recipes)
{
    DCHECK(g_ctx != NULL, "qjs_begin ran before qjs_init built the context");
    DCHECK(!g_begun, "qjs_begin ran twice — the frontier is seeded once and stepped thereafter");
    if (recipes && *recipes)
        DFAIL("qjs_begin was handed parked recipes — build the cold-tier resume that rebuilds each suspended "
              "flow's snapshot from its recipe and re-ranks it into the one frontier");
    engine_sched_begin(g_ctx, g_scripts.bodies, g_scripts.n, /*forking*/1);
    g_begun = 1;
}

/* One cooperative quantum of the ONE dispatch loop. ENGINE_STEP_YIELD leaves every flow exactly where it was;
 * ENGINE_STEP_DONE means the frontier is empty. */
QJS_EXPORT int qjs_step(void)
{
    DCHECK(g_begun, "qjs_step ran before the frontier was seeded");
    return engine_sched_step();
}

QJS_EXPORT void qjs_teardown(void)
{
    DCHECK(g_dom != NULL, "qjs_teardown ran on an instance that was never initialised");
    doc_scripts_free(&g_scripts);
    if (g_ctx) { JS_FreeContext(g_ctx); g_ctx = NULL; }
    if (g_rt)  { JS_FreeRuntime(g_rt);  g_rt  = NULL; }
    lxb_html_document_destroy(g_dom);
    g_dom = NULL;
    g_begun = 0;
}

/* ---- The capabilities this entry file does not yet reach ----------------------------------------------
 * Each one is a FEATURE THAT SHOULD EXIST. It aborts at its own entry naming what to build, because the
 * alternative — answering "" or 0 — is an engine that reports an empty pending set while a flow is parked on a
 * fetch that will never arrive, and a host that believes it. */

/* The URLs flows are parked on: a consumed reply is ALWAYS fetched, and a JS body additionally EXECUTED. */
QJS_EXPORT const char *qjs_pending(void)
{
    DFAIL("qjs_pending — build the parked-fetch register: a flow that awaits a host fetch records the URL here, "
          "and engine_pending_fetch delivers the body back into that flow's continuation");
    return "";
}

/* The lazily-loaded script URLs forced execution reached — the headline moat surface. */
QJS_EXPORT const char *qjs_chunks(void)
{
    DFAIL("qjs_chunks — build the lazy-script register: the script-load host-edge queues each discovered chunk "
          "URL, and engine_queue_script runs the fetched body in the CURRENT flow");
    return "";
}

QJS_EXPORT void qjs_provide(const char *url, const char *body)
{
    (void)url; (void)body;
    DFAIL("qjs_provide — build reply provision: enqueue a FORKING re-run of the fetch-initiating scope so a "
          "reply-gated branch forks WITH its example, not just a promise resolution");
}

/* Level-1 ranking: the host orders live per-page engines by their best flow's weight. */
QJS_EXPORT double qjs_top_weight(void)
{
    DFAIL("qjs_top_weight — expose the WFQ's top-of-frontier weight so the host can rank this engine against "
          "the other live documents");
    return 0;
}

/* The VALUE yield: run on until a parked flow — here, the runner-up ENGINE — outranks this one. */
QJS_EXPORT void qjs_set_yield_floor(double floor)
{
    (void)floor;
    DFAIL("qjs_set_yield_floor — build the value yield: the running flow suspends the moment its weight drops "
          "below this floor, which is the runner-up engine's best weight");
}

QJS_EXPORT void qjs_request_park(void)
{
    DFAIL("qjs_request_park — build the cold-tier park: serialize the lowest-value suspended tail to its "
          "recipes so the host can write them to IndexedDB and resume them in a later session");
}

QJS_EXPORT void qjs_emit_partial(void)
{
    DFAIL("qjs_emit_partial — build the streaming emit: append a fresh @RESULT for everything learned so far "
          "without disturbing the frontier");
}
