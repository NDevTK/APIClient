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
#include "browser/core/fetch/headers.h"
#include "browser/core/fetch/response.h"
#include "browser/core/fetch/request.h"
#include "browser/core/url/url.h"
#include "browser/core/frame/window_proxy.h"
#include "browser/core/events/broadcast_channel.h"
#include "browser/core/frame/window_message.h"
#include "browser/core/frame/remote_object.h"
#include "browser/core/html/html_iframe.h"
#include "browser/core/frame/navigable.h"
#include "browser/core/url/url_search_params.h"
#include "browser/core/html/form_data.h"
#include "browser/core/file/blob.h"
#include "browser/core/file/file_system.h"
#include "browser/core/file/file_system_handle.h"
#include "browser/core/file/file_system_writable.h"
#include "browser/core/file/storage_manager.h"
#include "browser/core/streams/readable_stream.h"
#include "browser/core/streams/queuing_strategy.h"
#include "browser/core/streams/writable_stream.h"
#include "browser/core/streams/transform_stream.h"
#include "browser/core/encoding/encoding.h"
#include "browser/core/encoding/text_stream.h"
#include "browser/core/dom/abort.h"
#include "browser/core/dom/observable.h"
#include "browser/core/html/unhandled_rejection.h"
#include "browser/core/dom/document.h"
#include "browser/core/dom/element.h"
#include "browser/core/idl_args.h"
#include "browser/core/events/event.h"
#include "browser/core/events/error_event.h"
#include "browser/core/events/message_event.h"
#include "browser/core/events/report_exception.h"
#include "browser/core/events/message_port.h"
#include "browser/core/xhr/xml_http_request.h"
#include "browser/core/structured_clone.h"
#include "browser/core/events/event_target.h"
#include "browser/core/realm.h"
#include "browser/core/frame/history.h"
#include "browser/core/frame/session_history.h"
#include "browser/core/frame/location.h"
#include "browser/core/frame/navigator.h"
#include "browser/core/frame/screen.h"
#include "browser/core/frame/viewport.h"
#include "browser/core/frame/visual_viewport.h"
#include "browser/core/frame/window.h"
#include "browser/core/css/media_query_list.h"
#include "browser/core/rendering/animation_frame.h"
#include "browser/core/rendering/page_reveal.h"
#include "browser/core/rendering/rendering.h"
#include "browser/core/timing/event_loop.h"
#include "browser/core/timing/timer.h"
#include "browser/core/loader/document_scripts.h"
#include "browser/core/loader/module_loader.h"
#include "solver/absent.h"
#include "solver/concolic.h"
#include "solver/cow.h"
#include "solver/endpoint.h"
#include "solver/engine.h"
#include "solver/flow.h"
#include "solver/world.h"
#include "solver/result.h"
#include "solver/solve.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#define QJS_EXPORT EMSCRIPTEN_KEEPALIVE
#else
#define QJS_EXPORT
#endif

/* One WASM instance per ORIGIN-KEYED AGENT CLUSTER (SECURITY.md), and these are the ROOT document's: the parse
 * the host handed over, its script inventory, and the one engine that runs the agent's whole frontier. Nothing
 * here is a registry — a same-origin child document is a second REALM (engine_child_realm, whose realms the
 * agent owns in navigable.c), and a cross-origin one is a second instance. */
static lxb_html_document_t *g_dom;
static DocScripts           g_scripts;
static unsigned             g_bundle_id;
static JSRuntime           *g_rt;
static JSContext           *g_ctx;
static int                  g_begun;
static int                  g_done;

/* THE AGENT AND THE DOCUMENT, SPLIT — see wpt_runner.c for the same split and the same reason. An AGENT is a
   JSRuntime: every class registration, the world registry, the flow frontier. A DOCUMENT is a JSContext in it.
   A SAME-ORIGIN CHILD NAVIGABLE IS A SECOND DOCUMENT IN THIS AGENT, so what a document IS has to be one
   description that runs twice — and while it was one block inside engine init, a second document could only
   have been a second INSTANCE, which for a same-origin child is the wrong answer (HTML's similar-origin window
   agent is one heap, and the corpus moves live nodes and live closures across that boundary).

   WHERE THE NEXT GAP IS: a component that builds its PROTOTYPE in its _init builds it in whichever realm was
   current, so a second realm shares it where §3.7 gives every realm its own. window.c is converted (its
   prototypes live in quickjs's per-context class-proto slot); the rest name themselves as they are reached. */
static void engine_agent_init(JSContext *ctx, const char *origin, const char *top_level_url)
{
    /* WHAT THE PLATFORM OWNS is WEB IDL's answer, not a list here: absent.c reads the generated
       browser/platform_names.h (every name [Exposed=Window]). A list typed at this spot covered 22 names
       of ~1300, so every interface it missed — Node, Element, Event, DOMException — was mistaken for
       server-injected app state and a branch on it forked instead of throwing. */
    url_init(ctx);
    usp_init(ctx);
    form_data_init(ctx);
    readable_stream_init(ctx);
    queuing_strategy_init(ctx);
    writable_stream_init(ctx);
    transform_stream_init(ctx);
    blob_init(ctx);
    encoding_init(ctx);
    text_stream_init(ctx);
    /* §2.7 BEFORE §7.2.5: Window.prototype is CHAINED to EventTarget.prototype, so the prototype has to exist
       before the window is installed. */
    event_target_init(ctx);
    window_init(ctx);
    /* §8.10.1's Navigator, DECLARED here and built per realm by the intrinsic it registers — its prototype,
       its interface object and the Window's one Navigator are §3.7 per-realm objects, so there is no install
       for this entry to call at document time. */
    navigator_init(ctx);
    /* THE ONE VIRTUAL FILESYSTEM, and the File System Standard over it. The MODEL goes first (its two roots are
       built at this pre-boot baseline, so no flow's creation becomes every sibling's); §2.5's stream is
       DECLARED after §5's WritableStream because its prototype chains to that one and core/realm.h runs the
       per-realm installs in declaration order; §2.2-§2.4's handles after the stream because `createWritable()`
       mints one; and §3's StorageManager after §8.10.1's Navigator, because `navigator.storage` is a partial
       interface member installed on the object that component builds. */
    file_system_init(ctx);
    fs_writable_init(ctx);
    fs_handle_init(ctx);
    storage_manager_init(ctx);   /* Storage §2's `navigator.storage`, and File System §3's getDirectory() */
    event_init(ctx);
    report_exception_init(ctx);
    message_port_init(ctx);
    xhr_init(ctx);   /* XHR §3, and §5's ProgressEvent under it */
    /* §7.2.4's Location and CSSOM VIEW §4.3's Screen, DECLARED here and built per realm by the intrinsics they
       register. Both are §3.7 per-realm objects — a prototype, an interface object and the one instance the
       Window is associated with — so there is no install for this entry to call at document time.
       §7.2.5.1 and §7.4. `window.open` returns a WindowProxy, and so does §4.8.5's contentWindow. The proxy
       class has to exist before any proxy is minted. */
    location_init(ctx);
    /* HTML §7.4.1's SESSION HISTORY and §7.2.5's History, DECLARED here and built per realm by the intrinsics
       they register. The state machine goes FIRST: realm.h runs the intrinsics in declaration order, and every
       member of History reads the record session_history's install builds. This is what a client-side router
       navigates with — `history.pushState()` is how React Router, Vue Router, Angular and every hand-rolled
       router change route — so without it a routing bundle threw on its first navigation and every route, lazy
       chunk and endpoint behind one went unexplored. */
    session_history_init(ctx);
    history_init(ctx);
    screen_init(ctx);   /* the responsive gate: screen.width decides which router a bundle uses */
    navigable_init(ctx);
    /* HTML §8.1.7's EVENT LOOP, before the task sources that are ordered by it: the virtual clock, §8.1.7.1's
       last render opportunity time and the insertion order a source breaks its ties by are the LOOP's, and
       they are per-flow heap state, so the record has to exist before any flow can write one. */
    event_loop_init(ctx);
    timer_init(ctx);
    window_proxy_init(ctx, origin);
    remote_object_init(ctx);   /* §7.2.5.1's object half: a peer's object crosses as a NAME */
    /* HTML §9.4.4 AND §9.5 — `window.postMessage` and `BroadcastChannel`. Both components are built and both
       were reachable only from the test runner: the SHIPPED engine had neither, so a bundle calling
       postMessage threw on a missing method and every message-driven code path behind it went unexplored.
       That is the largest class of code in a modern SPA that this engine was silently not entering. */
    window_message_init(ctx);
    broadcast_channel_init(ctx, origin);
    /* HTML §8.1.7.5: a rejection nobody handles is a page error, and it was invisible. */
    unhandled_rejection_init(ctx);
    /* HTML §8.1.7.3's IN-PARALLEL HALF — the rendering task source and "update the rendering". §8.9's map goes
       first because step 14 consumes it and §7.4.6.3 after Event, whose prototype PageRevealEvent chains to. */
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
    fetch_init(ctx);   /* §5/§6/§5.3 declare their per-realm prototypes here, not from the install */
    abort_init(ctx);
    observable_init(ctx);
    element_init(ctx);
    iframe_init(ctx);
    document_init(ctx);   /* §4.8.5: the slot a child navigable lives in */
    module_loader_install(g_rt);
    /* THE HOST'S NETWORK. SECURITY.md puts every byte of it behind the trusted chokepoint, so this host's
       answer is to PARK the request on the flow's pending register and let the trusted zone fetch it. */
    { static const FetchProvider P = { engine_pending_fetch_url }; fetch_set_provider(&P); }
    timer_set_script_sink(engine_queue_script);   /* HTML 8.6: a string handler is evaluated, as a flow */
    /* THE AGENT'S FIRST REALM IS A REALM. Every per-realm intrinsic the components above declared is built
       here, through the same one call a child navigable's realm makes — so the first document cannot get a
       different set from the rest, which is the whole failure mode of a hand-copied list.
       IT CARRIES THE ENVIRONMENT (core/realm.h). §8.1.3.5 reads the top-level creation URL to decide whether
       this realm is a SECURE CONTEXT, and Web IDL §3.3.13's members are installed or absent by that answer —
       so the environment has to exist before the first intrinsic does, which is why it arrives here. */
    realm_install_intrinsics(ctx, top_level_url);
}

/* ONE DOCUMENT — the per-realm half, run once per document including the first. */
/* `url` IS THE DOCUMENT'S ADDRESS AND `origin` IS ITS PRINCIPAL, and they are two different facts. This host
   passed the ORIGIN for both, so every document's §4.4 API BASE URL was `https://site` where the page was at
   `https://site/app/dashboard` — and the base URL is what `fetch("api/users")` resolves against, so the tool's
   own headline output named `https://site/api/users` for a request the page makes to `https://site/app/api/users`.
   The two other hosts already pass a real address; only the shipped one did not. */
static void engine_realm_install(JSContext *ctx, lxb_html_document_t *dom, const char *url, const char *origin,
                                 const char *csp, uint32_t doc_id, JSValueConst nav_proxy)
{
    JSValue g = JS_GetGlobalObject(ctx);

    /* THE PLATFORM GLOBALS, installed by their one caller rather than smuggled inside window_install.
       They are not browsing-context members and window.c does not own them. */
    url_install(ctx, g);
    usp_install(ctx, g);
    form_data_install(ctx, g);
    readable_stream_install(ctx, g);
    queuing_strategy_install(ctx, g);
    writable_stream_install(ctx, g);
    transform_stream_install(ctx, g);
    blob_install(ctx, g);
    encoding_install(ctx, g);
    text_stream_install(ctx, g);
    window_install(ctx, g, url);      /* window/self/frames/parent/top/opener/closed/origin, and name */
    timer_install(ctx, g);            /* setTimeout/setInterval/clearTimeout/clearInterval/queueMicrotask */
    event_install(ctx, g);            /* the Event interface object */
    message_event_install(ctx, g);    /* HTML 9.4.1: the event every messaging path dispatches */
    error_event_install(ctx, g);      /* HTML §8.1.4.6's report-an-exception fires one of these */
    message_port_install(ctx, g);     /* HTML 9.4.2/9.4.3 */
    xhr_install(ctx, g);              /* XHR §3, §5: XMLHttpRequest and ProgressEvent */
    window_message_install(ctx, g, origin);   /* HTML 9.4.4: window.postMessage, and §9.4.4's delivery task */
    broadcast_channel_install(ctx, g);        /* HTML 9.5: the named same-origin bus */
    structured_clone_install(ctx, g); /* HTML 2.7.3 */
    /* §2.7: the global reaches add/removeEventListener/dispatchEvent through Window.prototype ->
       EventTarget.prototype, which window_install chains it to. */
    event_target_install_handlers(ctx, g, EH_GLOBAL | EH_WINDOW);   /* window IS the global (7.2.2) */
    fetch_install(ctx, g);
    navigable_install(ctx, g, origin);
    unhandled_rejection_install(ctx, g);   /* PromiseRejectionEvent */
    animation_frame_install(ctx, g);       /* HTML §8.9: requestAnimationFrame/cancelAnimationFrame */
    page_reveal_install(ctx, g);           /* HTML §7.4.6.3: PageRevealEvent */
    media_query_list_install(ctx, g);   /* CSSOM VIEW §4.2/§7: matchMedia, MediaQueryList */
    abort_install(ctx, g);   /* AbortController/AbortSignal: fetch takes a signal, so a bundle mints one early */
    observable_install(ctx, g);
    /* NO navigator_install, NO location_install, NO screen_install: §8.10.1's Navigator, §7.2.4's Location and
       §4.3's Screen are per-realm intrinsics, so `navigator`, `clientInformation`, `location`, `screen` and
       their three interface objects are already on this global — realm_install_intrinsics put them there,
       through the ONE list every realm goes through rather than a line each host has to remember. */
    document_install(ctx, g, dom, url, csp, doc_id, nav_proxy);
    JS_FreeValue(ctx, g);
}

/* A SAME-ORIGIN CHILD NAVIGABLE'S REALM — a second JSContext in the SAME JSRuntime, which is what HTML's
   similar-origin window agent is. It gets the identical per-document install the first document got. */
/* THE ADDRESS IS A FACT ABOUT THE DOCUMENT, NOT A FETCH — the fetch is navigable.c's, through the document
   fetcher, and THIS HOST DECLARES NONE. That is not an omission to fill in with a shrug: this host's network is
   the trusted zone's and every request PARKS the running flow on a host-owed answer, so it cannot answer a
   synchronous fetch at all. §7.4's navigate has to become a scheduled work item here — the navigating flow
   parks on the response and resumes with it — and until it is, navigable.c asserts at the address rather than
   letting this host hand back the empty about:blank Document. It did exactly that, silently, behind a
   `(void)url;`: `window.open("/admin")` produced a popup whose scripts never ran and nothing in the output
   distinguished it from a page that had none. */
static JSContext *engine_child_realm(JSRuntime *rt, lxb_html_document_t *dom, const char *url,
                                     const char *top_level_url, const char *origin, const char *csp,
                                     uint32_t doc_id, JSValueConst nav_proxy)
{
    JSContext *ctx = JS_NewContext(rt);

    CHECK(ctx != NULL, "a same-origin child navigable's realm could not be created");
    CHECK(JS_AddIntrinsicDOMException(ctx) == 0, "the DOMException intrinsic failed to install in a child realm");
    /* §3.7: a realm gets its OWN intrinsics — the members on them run in the realm that DEFINED them, so a
       child sharing the agent realm's EventTarget.prototype would resolve every unqualified
       `addEventListener` against the PARENT's window. They come from the ONE list the components declared
       themselves into, so a component added anywhere is installed in every realm with no host to edit.
       §7.4 DECIDED THE CHILD'S TOP-LEVEL CREATION URL and handed it over — the creator's for a nested
       navigable, the navigable's own address for an auxiliary one — so this passes it rather than `url`,
       which would make an about:blank iframe of an http page a secure context. */
    realm_install_intrinsics(ctx, top_level_url);
    engine_realm_install(ctx, dom, url, origin, csp, doc_id, nav_proxy);
    return ctx;
}

/* PHASE 1 — parse and identify. No script runs, which is the whole point: the host reads the frontier key back
 * synchronously and looks up the prior session before any flow is seeded. */
/* `doc_id` NAMES THIS INSTANCE'S DOCUMENT, and the host names the ROOT one because the host is what knows
 * there is more than one at all. An instance is an ORIGIN-KEYED AGENT CLUSTER, so a same-origin iframe or popup
 * is a second REALM here and a CROSS-ORIGIN one is a peer instance — and that peer keys its segment of the
 * flow's world by this name. Two instances sharing a name would hand each other's flows the same segment: one
 * timeline wearing two names. Every document BELOW this one is named by the document that created it (world.h),
 * which is what lets §4.8.5 create a child navigable without asking anyone. */
/* THERE IS NO `code` ARGUMENT. The bridge used to hand the engine a concatenation of the page's scripts beside
 * the HTML, and this entry deliberately ignored it — identity and the script inventory come from the DOM's own
 * `<script>` scan, because a concatenation cannot represent per-script scope and shifts with an inline script
 * the page did not ship. An argument that is cast to `(void)` is not "documented as unused", it is a second
 * source of truth the next reader will wire up; it is gone from the ABI instead. */
/* `url` IS THE DOCUMENT'S ADDRESS, not its origin, and the ORIGIN IS DERIVED FROM IT HERE. The host used to
 * send the origin it had computed itself with `new URL(u).origin`, which is one fact arriving from two places
 * and the address arriving from none: §4.4's API base URL, `location.pathname` and `document.baseURI` were all
 * the bare origin. Deriving it here also makes it §4.7's real serialization, which url.c already implements. */
/* `top_level_url` IS HTML §8.1.3.1's TOP-LEVEL CREATION URL, and only the trusted zone can state it. One
 * WASM instance is one DOCUMENT regardless of origin, so this instance's document may itself be NESTED in a
 * document of another instance — and §8.1.3.5 decides whether this realm is a SECURE CONTEXT from the address
 * at the TOP of that chain, not from this document's own. Deriving it from `url` here would report an https
 * frame inside an http page as secure, which is exactly the ancestral hole Secure Contexts §4.2 exists to
 * close, and this engine cannot see its own embedder: the offscreen is the only zone that knows which instance
 * holds which document. For a top-level document the zone passes the address itself; for a document this
 * engine's peer created, it passes the field that peer put on its `navigable.create` notice. */
QJS_EXPORT int qjs_init(const char *html, const char *url, const char *doc_id, const char *csp,
                        const char *top_level_url)
{
    char *origin;

    CHECK(g_dom == NULL, "qjs_init ran twice in one WASM instance — one instance is one document");
    {
        UrlRecord rec;
        CHECK(url != NULL && *url && url_parse(&rec, url, strlen(url), NULL),
              "the host started this engine without a document ADDRESS — a document is loaded FROM somewhere, "
              "and every relative URL the page builds resolves against it");
        origin = url_serialize_origin(&rec);
        CHECK(origin != NULL, "the document address has no serializable origin");
        url_record_free(&rec);
    }
    CHECK(top_level_url != NULL && *top_level_url,
          "the host started this engine with no TOP-LEVEL CREATION URL — HTML §8.1.3.5 reads it to decide "
          "whether this document's realm is a secure context, and Web IDL §3.3.13's members exist or do not by "
          "that answer, so the platform surface this bundle runs against would be a guess. A top-level document "
          "passes its own address; a nested one passes its embedder's, which only the trusted zone knows");

    g_rt = JS_NewRuntime();
    CHECK(g_rt != NULL, "the runtime allocation failed: a dropped engine loses the whole frontier");
    JS_SetMaxStackSize(g_rt, 4 * 1024 * 1024);   /* align quickjs's overflow check with the 8MB wasm stack */
    g_ctx = JS_NewContext(g_rt);
    CHECK(g_ctx != NULL, "the context allocation failed");
    /* DOMException is a WEB IDL interface the DOM specs throw BY NAME — NotFoundError, NamespaceError,
       InvalidCharacterError — and the fork already implements the whole intrinsic. Not installing it left the
       components with nothing to throw, so each spec-mandated failure became a DCHECK saying "the spec throws
       here and this engine cannot": a documented gap standing in for a capability that existed. */
    CHECK(JS_AddIntrinsicDOMException(g_ctx) == 0, "the DOMException intrinsic failed to install");

    concolic_init(g_ctx);
    /* NOT A DEFAULT. An instance that does not know which document it is cannot be reached by a peer, and a
       silent 1 here would collide with every other instance that guessed the same. */
    DCHECK(doc_id != NULL && *doc_id, "the host started this engine without a document name — an agent is named by its root "
                     "peer instance names this document's flows by that id when it materializes their COW "
                     "segments, so an unnamed document cannot take part in cross-document time travel");
    flow_registry_init(doc_id);
    endpoint_init();
    solve_init(g_ctx);

    /* The two hook SETS the solver owns, each declared by its own component. They were struct literals here
       and again in test_forced.c, and the pair had drifted. */
    cow_install_time_travel_hooks(engine_gen_fork);
    concolic_install_hooks();
    concolic_install_source_overlay();   /* a SOLVER host: attacker-controlled values are symbolic sources */

    g_dom = lxb_html_document_create();
    CHECK(g_dom != NULL, "the document allocation failed");
    if (lxb_html_document_parse(g_dom, (const lxb_char_t *)(html ? html : ""),
                                html ? strlen(html) : 0) != LXB_STATUS_OK)
        CHECK_FAIL("the document parse failed — the DOM is the ground truth every flow reads");

    /* Identity and script inventory from the DOM's OWN executable scripts: a concatenation of them cannot
       represent per-<script> scope and would shift with an inline script the page did not ship. */
    g_bundle_id = document_bundle_id(g_dom);
    g_scripts   = document_exec_scripts(g_dom);

    /* The web-platform surface, installed on the BASELINE — before any flow runs, so these globals are
       pre-flow state and never land in a delta. Each is a real component under browser/; what is not built
       yet is absent, and the page's own throw on reading it names the next one to write. */
    engine_agent_init(g_ctx, origin, top_level_url);
    /* §7.4 CALLS BACK HERE FOR A SAME-ORIGIN CHILD: a same-origin document is a second REALM in this heap
       (HTML's similar-origin window agent), and what the platform surface of a document of THIS build is, is
       this file's answer and nobody else's. */
    navigable_set_realm_builder(engine_child_realm);
    /* THE ROOT NAVIGABLE IS THIS HOST'S, so its §7.2.5.1 proxy is minted here — one per navigable, made by
       whoever owns the navigable, which for the root is the host that named it. */
    {
        /* NULL: THIS HOST DOES NOT KNOW THE NAVIGABLE'S NAME. The document was navigated to by the real
           browser, and a cross-origin opener may have set `name` before navigating — which is exactly how
           window.name became an attacker source. The read is concolic until something states it. */
        JSValue root_proxy = window_proxy_new_self(g_ctx, world_local_doc(), NULL);
        CHECK(!JS_IsException(root_proxy), "the root navigable's WindowProxy could not be allocated");
        engine_realm_install(g_ctx, g_dom, url, origin, csp, world_local_doc(), root_proxy);
        JS_FreeValue(g_ctx, root_proxy);
    }
    /* The surface is installed, so every member the platform has is declared — a declaration from here on is a
       per-wrapper or per-flow mint, and that is what the pool asserts against. */
    idl_args_seal();

    free(origin);
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
              "flow's snapshot from its recipe and re-ranks it into the one frontier. A flow suspended inside a "
              "step machine names its rest point by the machine's ALGORITHM and the SPEC STEP it is resting at "
              "(JSTrampStepDef.steps), never by the stage INDEX: an index means whatever this build's stage "
              "constants say, so a recipe carrying one resumes at a different step of the same algorithm the "
              "first time those constants move, silently");
    engine_set_stall_hook(qjs_owed);
    engine_sched_begin(g_ctx, g_scripts.bodies, g_scripts.srcs, g_scripts.n, /*forking*/1);
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
    return result_json(g_ctx);
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
    DCHECK(*engine_host_requests() == '\0',
           "qjs_teardown with SYNCHRONOUS requests still owed — a flow blocked on one is suspended mid-frame "
           "with its snapshot intact, and tearing down drops it exactly where the pending assert above says "
           "not to. Answer them, or step to DONE, before ending the session");
    doc_scripts_free(&g_scripts);
    /* THE ORDER AND THE MEMBERSHIP OF THIS LIST ARE THE OTHER ENTRY'S, VERIFIED, and this one had drifted from
       it — which nothing could see, because the gate that walks gc_obj_list runs the OTHER entry's main().
       Four frees were missing here (§8.1.7.5's rejected-promise list and its slot key, the solver's solve and
       endpoint tables, the flow registry) and so was the collection, so the shipped engine aborted at teardown
       on `list_empty(&rt->gc_obj_list)` and every finding it had produced was discarded as a crashed
       instance's. MEASURED on the live harness: a page analysed to "No findings" for that reason alone. */
    solve_free();
    endpoint_free();
    rendering_free(g_ctx);
    page_reveal_free(g_ctx);
    media_query_list_free(g_ctx);
    viewport_free();
    visual_viewport_free();
    animation_frame_free(g_ctx);
    timer_free(g_ctx);        /* §8.6's declaration; each global's map went with its realm */
    event_loop_free(g_ctx);   /* §8.1.7's own record — the virtual clock and the moments beside it */
    unhandled_rejection_free(g_ctx);
    abort_free(g_ctx);
    observable_free(g_ctx);
    document_free(g_ctx);   /* the Document and the window it fires `load` at — both HELD across the lifecycle */
    iframe_free(g_ctx);
    element_free(g_ctx);    /* the wrapper identity table and the DOM interface prototypes */
    event_target_free(g_ctx);
    realm_intrinsics_free();   /* the DECLARATIONS are the agent's; each realm's prototypes went with it */
    window_message_free(g_ctx);
    broadcast_channel_free(g_ctx);
    message_port_free(g_ctx);
    xhr_free(g_ctx);
    report_exception_free(g_ctx);
    event_free(g_ctx);
    headers_free(g_ctx);    /* Headers.prototype and the name it interned */
    url_free(g_ctx);
    usp_free(g_ctx);
    transform_stream_free(g_ctx);
    writable_stream_free(g_ctx);
    queuing_strategy_free(g_ctx);
    readable_stream_free(g_ctx);
    blob_free(g_ctx);
    fs_handle_free();
    fs_writable_free();
    storage_manager_free();
    file_system_free(g_ctx);   /* the two roots are the agent's, and they outlive no agent */
    encoding_free(g_ctx);
    text_stream_free(g_ctx);
    form_data_free(g_ctx);        /* URLSearchParams.prototype */
    response_free(g_ctx);
    request_free(g_ctx);   /* Response.prototype — one object, held for the runtime's life */
    idl_args_free(g_ctx);   /* the dictionary member atoms the declaration pool interned */
    navigable_free(g_ctx);
    navigator_free();   /* the two per-realm slot ids; each realm's Navigator went with its context */
    location_free();
    session_history_free();
    history_free();
    screen_free();
    window_free(g_ctx);
    remote_object_free(g_ctx);
    window_proxy_free(g_ctx);   /* the shared §7.2.5.1 prototype every proxy is chained to */
    flow_registry_free(g_ctx);
    /* THE WORLD REGISTRY holds a COW DELTA per foreign world, and a delta holds the JSValues it captured — so
       leaving it behind pins every object any cross-document flow ever wrote through. */
    world_registry_free(g_ctx);
    /* THE COLLECTION IS PART OF THE TEARDOWN, not an optimisation: a run leaves flow-local garbage that is
       unreachable but not yet collected, and gc_obj_list counts it exactly as it counts a real leak. */
    if (g_rt) JS_RunGC(g_rt);
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

/* THE REPLY CROSSES AS TEXT AND CARRIES ITS TYPE — JSON, exactly as qjs_host_answer's does and for the same
   reason. It used to cross as the BODY'S BYTES and nothing else, so everything the trusted zone had actually
   seen was thrown away at this line and re-invented on the other side: the engine's drain built a reply with
   status 200, status message "OK", no headers, and no URL LIST — which is what `response.url` and
   `response.redirected` are, so every reply in the extension reported no redirect however many the fetch had
   followed. The record is `{status, statusText, headers: [[name, value], …], body, urlList: [url, …]}`, which
   is the SAME record fetch_reply_new builds for the hosts that fetch in C; JSON `null` is a NETWORK ERROR, and
   the delivery machine already rejects with the TypeError §5.6 names.
   ONE delivery for every parked request. A fetch's reply, the DOCUMENT's own external script and a lazy CHUNK's
   source all settle a park the flow registered before it suspended, and one engine_provide fills every entry
   naming that URL whatever its kind — so there is ONE record and the kinds read the fields they need. */
QJS_EXPORT void qjs_provide(const char *url, const char *reply)
{
    DCHECK(g_begun, "a reply was provided to an engine that never ran");
    DCHECK(reply != NULL, "a reply was provided with no text at all — a network error is the JSON `null`, "
                          "which is a value the engine's delivery distinguishes from a reply it never got");
    {
        JSValue v = JS_ParseJSON(g_ctx, reply, strlen(reply), "<reply>");
        int n;
        if (JS_IsException(v)) {
            JS_FreeValue(g_ctx, JS_GetException(g_ctx));
            /* ABORTS in dev, which is the point: a host sending anything but its reply record through this
               edge is the bug. In release there is no reply to deliver, and "no reply" is a NETWORK ERROR
               rather than an empty 200 — the delivery machine rejects with §5.6's TypeError. */
            DFAIL("the host provided a reply that is not JSON — the trusted zone stringifies its reply record, "
                  "and a bare body sent through this edge is a host still delivering only bytes");
            v = JS_NULL;
        }
        n = engine_provide(g_ctx, url, v);
        JS_FreeValue(g_ctx, v);
        if (n == 0)
            DFAIL("a reply was provided for a URL no flow is parked on — the host's pending/provide pairing is "
                  "off, and resolving nothing would leave the flow that IS parked waiting forever");
    }
}

/* WHAT ONLY THE TRUSTED ZONE CAN ANSWER, as `id<TAB>op` lines. A cross-document operation — creating a
   navigable, reading through a WindowProxy — is answered by the instance holding that document, and only the
   offscreen knows which instance that is. The asking flow is SUSPENDED until qjs_host_answer lands, so this
   list is pulled every step alongside qjs_pending, and an unanswered request parks that flow indefinitely
   while every sibling keeps running. */
QJS_EXPORT const char *qjs_host_requests(void)
{
    DCHECK(g_begun, "qjs_host_requests was asked of an engine that never ran");
    return engine_host_requests();
}

/* WHAT THE ENGINE TELLS THE TRUSTED ZONE, one-way, as `op` lines — DRAINED by the read. A document created
   here (§4.8.5's child navigable, §7.4's popup) is announced rather than negotiated: the name is minted in this
   instance, so there is nothing to ask and nothing to wait for, and the host's job is to provision an instance
   under that name and route to it. Pulled every step beside qjs_host_requests; a notice the host drops is a
   document nothing runs, and every read through it parks its flow forever. */
QJS_EXPORT const char *qjs_host_notices(void)
{
    DCHECK(g_begun, "qjs_host_notices was asked of an engine that never ran");
    return engine_host_notices();
}

/* THE ANSWER CROSSES AS TEXT AND CARRIES ITS TYPE — JSON, so `null` is not the string "null", `0` is not the
   string "0", and an answer with STRUCTURE can exist at all. It used to cross as a bare string, which could
   express exactly one of the shapes the seam already needs: §7.4 step 14's load is answered with `{body, csp}`
   because a policy is a property of THE RESPONSE, and a cross-origin `otherW.length` is a NUMBER that a page
   distinguishes from "0". That is the same sentence SECURITY.md states for the cross-instance encoder, applied
   to the seam one layer down. Redefining it costs nothing: the bridge has never answered a request, so this
   entry had no caller to break.
   AND AN ANSWER IS A COMPLETION, NOT A VALUE — `completion` is 0 for a normal one and 1 for a THROW whose
   value is the JSON. The offscreen relays a cross-agent operation performed by ANOTHER WASM INSTANCE, and that
   instance answers BY RUNNING A PROGRAM: an IDL accessor, a page's setter, a page's own function. With no
   parameter for it the only completion this entry could express was a normal one, so a peer's throw would
   arrive at the asking flow as `undefined` and the page's `try`/`catch` around the operation would never run —
   the identical hole the cross-instance grammar had one layer up. It is a PARAMETER and not a second entry
   point beside this one, so a zone answering a request has to say which completion it is answering with. */
QJS_EXPORT void qjs_host_answer(unsigned req, const char *json, unsigned completion)
{
    DCHECK(g_begun, "an answer was provided to an engine that never ran");
    DCHECK(completion == ENGINE_COMPLETION_NORMAL || completion == ENGINE_COMPLETION_THROW,
           "the trusted zone answered a request with a completion type ECMA-262 6.2.4 does not have — an "
           "operation performed in another instance returns or throws, and nothing else crosses a call site");
    {
        JSValue v = json ? JS_ParseJSON(g_ctx, json, strlen(json), "<host-answer>") : JS_UNDEFINED;
        /* A MALFORMED ANSWER IS THE HOST'S BUG, not the page's. Delivering the exception instead would surface
           in the asking flow as if the DOCUMENT had thrown, which is a lie about whose code was wrong. */
        DCHECK(!JS_IsException(v), "the host answered a synchronous request with text that is not JSON — the "
                                   "answer crosses as JSON so that it carries its type, and a bare string is "
                                   "not one (it is the JSON text `\"...\"`)");
        /* Routed to ONE call site by id — never broadcast the way a fetched body is, because the answer was
           computed under the ASKING FLOW's world. A zero return means that flow is gone, which is not an
           error: nobody is waiting on the answer. */
        engine_host_answer(g_ctx, (uint32_t)req, v, (int)completion);
        JS_FreeValue(g_ctx, v);
    }
}

/* THE RETURN PATH FOR A NOTICE — the entry that was missing, and its absence is why the shipped extension could
   send a cross-document message and never deliver one. `record` is the notice VERBATIM as the emitting instance
   wrote it (`qjs_host_notices`), routed here by the offscreen because this instance holds the document the
   record names; `sender_origin` is that instance's serialized origin, which ONLY the trusted zone may state —
   SECURITY.md keys authorization on what the trusted zone knows for exactly this reason, and an origin the
   untrusted engine supplied for a foreign message would defeat every `event.origin` check in every bundle.
   The delivery becomes a FLOW on the one frontier, so it is ordered, preemptible and parkable like all the
   others; nothing runs inside this call. */
QJS_EXPORT void qjs_route(const char *record, const char *sender_origin)
{
    DCHECK(g_begun, "a record was routed to an engine whose frontier was never seeded — there is no scheduler to "
                    "run the delivery, so it would be dropped");
    engine_route(g_ctx, record, sender_origin);
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
    printf("@RESULT %s\n", result_json(g_ctx));
    fflush(stdout);
}
