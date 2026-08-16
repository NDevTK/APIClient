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
#include "browser/core/frame/navigation_params.h"
#include "browser/core/frame/secure_context.h"
#include "browser/core/url/url_search_params.h"
#include "browser/core/html/form_data.h"
#include "browser/core/file/blob.h"
#include "browser/core/file/file_system.h"
#include "browser/core/file/file_system_access.h"
#include "browser/core/file/file_picker.h"
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
#include "browser/core/geometry/dom_rect.h"
#include "browser/core/geometry/dom_rect_list.h"
#include "browser/core/idl_args.h"
#include "browser/core/idl_async_iter.h"
#include "browser/core/events/event.h"
#include "browser/core/events/error_event.h"
#include "browser/core/events/message_event.h"
#include "browser/core/events/report_exception.h"
#include "browser/core/events/message_port.h"
#include "browser/core/xhr/xml_http_request.h"
#include "browser/core/structured_clone.h"
#include "browser/core/events/event_target.h"
#include "browser/core/platform.h"
#include "browser/core/realm.h"
#include "browser/core/frame/history.h"
#include "browser/core/frame/navigation.h"
#include "browser/core/frame/navigation_history_entry.h"
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
#include "solver/req2proto.h"
#include "solver/flow.h"
#include "solver/quantum.h"
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

/* THE AGENT AND THE DOCUMENT, SPLIT. An AGENT is a JSRuntime: every class registration, the world registry,
   the flow frontier. A DOCUMENT is a JSContext in it. A SAME-ORIGIN CHILD NAVIGABLE IS A SECOND DOCUMENT IN
   THIS AGENT, so what a document IS has to be one description that runs twice.

   AND IT IS NOW ONE DESCRIPTION FOR THE THREE HOSTS AS WELL — core/platform.h. This file used to write out by
   hand which components an agent declares and which a document installs, and so did wpt_runner.c and
   test_forced.c; three copies is the point at which a list stops being maintained, and they had already
   drifted (the WPT gate had no `navigator` and therefore none of Permissions, Storage, File System Access or
   UserActivation, and reported numbers for those areas as if it had run them). What is left in a host is only
   what is genuinely the host's: its EDGES — WHO answers the network, WHO evaluates a string handler — which
   are parameters rather than presence, and which each component asserts for itself at its first use. */
static void engine_agent_init(JSContext *ctx, const char *origin, const char *top_level_url, bool requests_oac)
{
    PlatformAgent agent;

    /* THE HOST'S NETWORK. SECURITY.md puts every byte of it behind the trusted chokepoint, so this host's
       answer is to PARK the request on the flow's pending register and let the trusted zone fetch it.
       fetch.c aborts on a fetch issued with no provider, which is what asserts this line is here. */
    { static const FetchProvider P = { engine_pending_fetch_url }; fetch_set_provider(&P); }
    timer_set_script_sink(engine_queue_script);   /* HTML 8.6: a string handler is evaluated, as a flow */

    agent.origin = origin;
    agent.top_level_url = top_level_url;
    /* §7.5.1's requestsOAC, decided by the browser component that reads the response (§7.4.6's navigation
       params) and merely CARRIED here — this host reads no header, which is the same split as the origin
       beside it: the zone states bytes, the engine decides what they mean. */
    agent.requests_oac = requests_oac;
    platform_agent_init(ctx, &agent);
}

/* ONE DOCUMENT — the per-realm half, run once per document including the first.
   `url` IS THE DOCUMENT'S ADDRESS AND `origin` IS ITS PRINCIPAL, and they are two different facts. This host
   passed the ORIGIN for both, so every document's §4.4 API BASE URL was `https://site` where the page was at
   `https://site/app/dashboard` — and the base URL is what `fetch("api/users")` resolves against, so the tool's
   own headline output named `https://site/api/users` for a request the page makes to
   `https://site/app/api/users`. They are two FIELDS of the one document record now, which is what makes that
   substitution unspellable rather than merely fixed. */
static void engine_realm_install(JSContext *ctx, lxb_html_document_t *dom, const char *url, const char *origin,
                                 const char *csp, const char *csp_self_origin, SandboxFlags sandbox_flags,
                                 uint32_t doc_id, JSValueConst nav_proxy)
{
    PlatformDocument doc;
    JSValue g = JS_GetGlobalObject(ctx);

    doc.dom = dom;
    doc.url = url;
    doc.origin = origin;
    doc.csp = csp;
    doc.csp_self_origin = csp_self_origin;
    doc.sandbox_flags = sandbox_flags;
    doc.doc_id = doc_id;
    doc.nav_proxy = nav_proxy;
    platform_document_install(ctx, g, &doc);
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
                                     const char *csp_self_origin, SandboxFlags sandbox_flags, uint32_t doc_id,
                                     JSValueConst nav_proxy)
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
    engine_realm_install(ctx, dom, url, origin, csp, csp_self_origin, sandbox_flags, doc_id, nav_proxy);
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
/* `headers` IS THE NAVIGATION RESPONSE'S HEADER LIST, and it replaces the single `csp` argument that used to
 * stand here. That argument was the whole of what a Document in this engine could be created with, and three
 * separate capabilities were stuck behind it: §7.1.7's policy container has an EMBEDDER POLICY item that had
 * no writer, §7.5.1's creation table gives a Document an OPENER POLICY row that did not exist, and §7.5.1's
 * own `Origin-Agent-Cluster` read never happened — so §8.1.2.2's agent cluster key was the SITE for every
 * document this engine has ever built, and `originAgentCluster` answered `false` without ever having asked.
 * One header cannot be widened into three by adding three arguments: the standard reads a HEADER LIST, several
 * algorithms read different names out of the same one, and Fetch's own `get` is what decides what a REPEATED
 * header means (§7.1.4.1's table turns on `require-corp, require-corp` failing to parse as an item). So the
 * whole list crosses, as HTTP field lines — `name: value`, one per line — and core/fetch/headers.c parses it
 * into the list Fetch defines. An empty or NULL block is a response that carried no headers.
 *
 * IT IS A LIST OF LINES AND NOT A MAP FOR THAT SAME REASON. The trusted zone holds a `Headers` object, whose
 * iteration already combines repeats the way Fetch's `get` does; a map keyed by name would arrive here having
 * silently made a two-header COEP look like a one-header COEP, which is the difference between a document a
 * browser isolates and one it does not.
 *
 * THE ENGINE CANNOT FORGE THESE HEADERS, and that is structural rather than checked. There is no ABI entry
 * that writes a response header and no page-reachable member that adds one — a `<meta>` element can carry a
 * CSP (CSP §3.3, merged by core/dom/document.c) and nothing else, which is the standard's own answer for why
 * COOP, COEP and `Origin-Agent-Cluster` are response headers only. The list is read ONCE, here, into the
 * decisions §7.5.1 creates the Document from, and then freed: nothing the bundle runs can reach it afterwards.
 * SECURITY.md's boundary is unchanged by this — the zone that captured the response states the bytes, exactly
 * as it states the address, the origin and the top-level creation URL. */
QJS_EXPORT int qjs_init(const char *html, const char *url, const char *doc_id, const char *headers,
                        const char *top_level_url)
{
    char *origin;
    HeaderList response_headers;
    NavigationParams np;

    /* THIS ENTRY ROOTS THE AGENT AT ONE DOCUMENT, which is not the same statement as "one instance is one
       document" — the sentence that used to stand here and the model the host was keyed on. An instance is an
       ORIGIN-KEYED AGENT CLUSTER and holds one realm per same-origin document (engine_child_realm below), so
       what may not happen twice is the ROOTING: the runtime, the class registrations, the world registry and
       the frontier are the agent's and exist once. A SECOND document of this cluster that this agent did not
       itself create — the trusted zone reporting a same-origin frame it did not model, or a navigation
       replacing the root — has no entry to arrive through yet: it needs a `qjs_join` beside this one that
       parses a second document, builds its realm through engine_child_realm, and seeds its scripts on the SAME
       frontier. Calling this twice is not that entry, and it would silently be a second agent. */
    CHECK(g_dom == NULL, "qjs_init ran twice in one WASM instance — it ROOTS the agent, and a second document "
                         "of this origin-keyed cluster joins through the join entry rather than re-rooting it");
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

    /* THE RESPONSE, READ ONCE, BEFORE ANYTHING IS BUILT OUT OF IT. §7.4.6's navigation params are what §7.5.1
       creates a Document from, and they are decided at the RESPONSE and carried — never read back off the
       Document later, which is CLAUDE.md's rule about an operation taking its inputs with it and is why the
       standard splits these into two algorithms.
       THE SECURE-CONTEXT ANSWER IS §8.1.3.5's OVER THE TOP-LEVEL CREATION URL, asked here rather than of a
       realm because §7.1.3's and §7.1.4's obtains and §7.5.1's `requestsOAC` all run BEFORE the realm whose
       environment it is exists — the standard calls that environment the RESERVED one for exactly this reason.
       ZERO IS THE ROOT NAVIGABLE'S TARGET SNAPSHOT SANDBOXING FLAGS, and it is the spec's answer rather than a
       placeholder: this is the navigable the INSTANCE STARTED IN, a top-level traversable with no embedder
       element, so §7.1.5 answers its creation flags from the POPUP sandboxing flag set — which begins empty and
       which only §7.1's rules for choosing a navigable ever fill. Nothing chose this one. The other half of
       §7.4.5's union, this response's CSP-derived flags, is what navigation_params computes. */
    memset(&response_headers, 0, sizeof response_headers);
    header_list_parse_field_lines(&response_headers, headers);
    navigation_params_from_response(&np, &response_headers, 0,
                                    secure_context_url_potentially_trustworthy(top_level_url));
    header_list_free(&response_headers);

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
    req2proto_init();
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
    engine_agent_init(g_ctx, origin, top_level_url, np.requests_oac);
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
        /* §7.5.1's Document, from the navigation params decided above and not from anything read back off the
           response — which this host no longer holds. §7.4.5's final sandboxing flag set and §7.1.7 step 3's
           CSP list are both `np`'s, computed where a response is read (core/frame/navigation_params.c); this
           file's remaining job is to say WHICH navigable and WHICH realm, which is a host fact. */
        /* CSP §2.2.2's SELF-ORIGIN — "response's URL's origin" — which for this entry is the origin of the
           address the response was fetched from, serialized above. It is stated as its own argument rather
           than taken from `origin` beside it because the two are different facts (a Document's principal and
           the origin its policy resolves `'self'` against), and they agree here for a reason this entry can
           name: this document IS the response's, loaded from `url`. */
        engine_realm_install(g_ctx, g_dom, url, origin, np.csp, origin, np.sandbox_flags,
                             world_local_doc(), root_proxy);
        JS_FreeValue(g_ctx, root_proxy);
    }
    /* The surface is installed, so every member the platform has is declared — a declaration from here on is a
       per-wrapper or per-flow mint, and that is what the pool asserts against. */
    idl_args_seal();

    /* THE RESPONSE IS GONE NOW, and that is the point at which it stops being reachable at all. Its decisions
       are on the Document and on the agent's cluster; nothing the bundle runs can ask for a header again. */
    navigation_params_free(&np);
    free(origin);
    return 0;
}

/* The frontier KEY's document half. Pure scan, so the host may ask the instant qjs_init returns. */
QJS_EXPORT unsigned qjs_bundle_id(void)
{
    DCHECK(g_dom != NULL, "qjs_bundle_id was asked before the document was parsed");
    return g_bundle_id;
}

/* THERE IS NO STALL HOOK HERE ANY MORE, and the one that stood here is why: it answered
   `*engine_pending_urls() != 0` and never engine_host_requests(), so this host told the scheduler its frontier
   was EXHAUSTED whenever every member was suspended inside a cross-instance read with no fetch outstanding.
   The scheduler then seeded candidates over those flows and closed the session on top of them. Both C drivers
   asked both registers; the SHIPPED one asked half, which is §Testing's own sentence about which entry point
   rots. The question is the engine's, over the engine's registers (engine.h's engine_host_owes), and no host
   restates it. */

/* PHASE 2 — seed the frontier. */
QJS_EXPORT void qjs_begin(const char *recipes)
{
    DCHECK(g_ctx != NULL, "qjs_begin ran before qjs_init built the context");
    DCHECK(!g_begun, "qjs_begin ran twice — the frontier is seeded once and stepped thereafter");
    /* THE PARKED RESIDUE SEEDS THE FRONTIER, INSTEAD OF THE BOOT FLOW — solver/cold.h. What the host stored is
       a set of RECIPES: each names the path one suspended flow had taken, as arms over shared frozen segments,
       and each resumes by REPLAYING that path while it re-runs this document from its first script. The flow's
       delta, its DOM writes, its suspended frames, its constraint, its chunks and the replies it was owed are
       all regenerated by that replay — which is also how §Time-travel-resume gets what it asks for, a flow that
       carries its own path forward while its example VALUES track today's server. */
    engine_sched_begin(g_ctx, g_scripts.bodies, g_scripts.srcs, g_scripts.types, g_scripts.n,
                       /*forking*/1, recipes);
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
    /* THE OTHER HALF OF THE SLICE INVARIANT, and it belongs HERE because this is the only entry in this ABI
       that opens one. preempt_hook asserts that the engine may only EXECUTE inside a slice; this asserts that
       the slice is CLOSED before the thread goes back to the host — and together they are what makes every
       other entry below (provide, route, perform, answer, result, teardown) host-time BY CONSTRUCTION rather
       than by inspection: a slice cannot survive this line, so anything one of them reaches that consults the
       scheduler's policy crashes at the consultation instead of being answered against a slice belonging to
       nothing. Asserted before the STALLED fold, so the stall path is covered too — it is the exit the reply
       entries are called on. engine_sched_step brackets exactly one slice around exactly one call, so a hit
       here means something opened a slice OUTSIDE that bracket. */
    DCHECK(!quantum_slice_open(),
           "qjs_step is returning to the host with the cooperative quantum's slice still OPEN — the host now "
           "pumps its port, streams findings and delivers replies on time the scheduler believes a flow is "
           "holding, and the CPU edge is still armed over it");
    /* AND THE FLOW STAMP IS DOWN, which is the assert that would have caught the defect it now guards on the
       day it was written. Every entry below this one runs HERE, on the host's own time, and each of them
       creates objects: qjs_provide parses a reply record and dups it onto every parked flow's pending
       register, qjs_host_answer parses an answer, qjs_route and qjs_perform build a delivery. With the stamp
       still up those objects carry the running flow's generation, so `JS_ObjFlowGen(obj) > d->fork_gen` skips
       them in every delta forked before — a later write by any flow that shares them is captured nowhere and
       outlives that flow's unapply. The engine cannot see that from the inside (nothing crashes; the refcount
       keeps the object alive and the value is simply wrong), so it is asserted at the boundary where the
       ownership changes hands. */
    DCHECK(JS_FlowGen() == 0,
           "qjs_step is returning to the host with the flow stamp still up — every object the host now creates "
           "would be stamped as belonging to the flow that just yielded, and a write to one of them by any "
           "OTHER flow that shares it is skipped by that flow's delta and survives its rewind");
    /* …AND SO IS THE CAPTURE ROUTE, which is the OTHER half of that one fact and the half the paragraph above
       did not have. Putting the stamp down is what makes the host's objects BASELINE — and a baseline object is
       the SHARED arm of the capture test, so with the route still pointed at the flow that just yielded, every
       property the four entries below write on a record they have just built is recorded as a CREATION in that
       flow's head. The next context switch away from it runs the unapply, which DELETES a creation: the reply
       record reaches the flow it was fetched for as `{}` — no urlList, no status, no headers, no body — with
       its refcount intact and nothing at all to say what happened to it. The two asserts are opposite failures
       of the same boundary, so they stand together and neither is enough alone. */
    DCHECK(cow_current() == NULL,
           "qjs_step is returning to the host with the last flow's CAPTURE ROUTE still up — every field the "
           "host writes on a record it builds is recorded as a creation in that flow's COW head, and its next "
           "context switch deletes them, so the reply arrives at its delivery stripped to an empty object");
    if (r == ENGINE_STEP_STALLED)
        return ENGINE_STEP_YIELD;   /* the bridge speaks two values; a stall is "call me again", same as a slice */
    /* TWO VALUES, AND THIS ENTRY IS WHERE THAT IS TRUE. The scheduler has three codes and the fold above is
       what makes them two, so a fourth code added there would arrive at a host whose branches are written
       against this ABI — and the bridge's own third branch (a NEED_FETCH that no version of the scheduler has
       ever returned) is what a host does with a value it was never given: it writes a plausible one and the
       path behind it is never taken. */
    DCHECK(r == ENGINE_STEP_DONE || r == ENGINE_STEP_YIELD,
           "the scheduler answered a step with a code this ABI does not carry — the host branches on DONE and "
           "YIELD alone, so a third value reaches it as whichever branch happens to be the fallback");
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
    /* …UNLESS THE FRONTIER WAS PAGED OUT, which is the one way tearing down over an owed reply is not a drop.
       A parked flow's recipe replays the code that ISSUED the request, so the request is re-made in the
       session that resumes it and answered with today's reply — which is exactly what §Time-travel-resume
       means by a resumed flow re-deriving its values from current sources. The assert is about the flow being
       LOST, so it asks whether it was written down, not whether it was answered. */
    DCHECK(engine_frontier_paged() || *engine_pending_urls() == '\0',
           "qjs_teardown with replies still owed — every flow parked on one is dropped with its continuation. "
           "Provide them, step to DONE, or park the frontier, before ending the session");
    DCHECK(engine_frontier_paged() || *engine_host_requests() == '\0',
           "qjs_teardown with SYNCHRONOUS requests still owed — a flow blocked on one is suspended mid-frame "
           "with its snapshot intact, and tearing down drops it exactly where the pending assert above says "
           "not to. Answer them, step to DONE, or park the frontier, before ending the session");
    doc_scripts_free(&g_scripts);
    /* THE ORDER AND THE MEMBERSHIP OF THIS LIST ARE THE OTHER ENTRY'S, VERIFIED, and this one had drifted from
       it — which nothing could see, because the gate that walks gc_obj_list runs the OTHER entry's main().
       Four frees were missing here (§8.1.7.5's rejected-promise list and its slot key, the solver's solve and
       endpoint tables, the flow registry) and so was the collection, so the shipped engine aborted at teardown
       on `list_empty(&rt->gc_obj_list)` and every finding it had produced was discarded as a crashed
       instance's. MEASURED on the live harness: a page analysed to "No findings" for that reason alone. */
    /* THE PLATFORM'S OWN LIST, UNDONE — every component's agent-lifetime release, in one call, so the next one
       to hold agent state is not a fourth copy of a list that has already drifted once. */
    platform_agent_free();
    solve_free();
    endpoint_free();
    req2proto_free();
    rendering_free(g_ctx);
    page_reveal_free(g_ctx);
    media_query_list_free(g_ctx);
    viewport_free();
    visual_viewport_free();
    animation_frame_free(g_ctx);
    timer_free(g_ctx);        /* §8.6's declaration; each global's map went with its realm */
    event_loop_free(g_ctx);   /* §8.1.7's own record — the virtual clock and the moments beside it */
    /* §8.1.7.5's rejection list is NOT freed here any more — it is a row on core/platform.h's release column,
       run by the platform_agent_free above. It was a line in this list and in nobody else's, and the host that
       did not have it aborted every file it ran on the runtime's leak walk. Every other line below is still a
       hand-copied teardown, which is the same defect waiting: they belong on that column too. */
    abort_free(g_ctx);
    observable_free(g_ctx);
    document_free(g_ctx);   /* the Document and the window it fires `load` at — both HELD across the lifecycle */
    iframe_free(g_ctx);
    element_free(g_ctx);    /* the wrapper identity table and the DOM interface prototypes */
    /* GEOMETRY INTERFACES, after the CSSOM VIEW members that return one: §4's list holds a private Symbol
       and an interned atom, and §3 holds only pool ids. */
    dom_rect_list_free(g_ctx);
    dom_rect_free();
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
    file_picker_free();
    file_system_access_free();
    fs_handle_free();
    fs_writable_free();
    file_system_free(g_ctx);   /* the two roots are the agent's, and they outlive no agent */
    encoding_free(g_ctx);
    text_stream_free(g_ctx);
    form_data_free(g_ctx);        /* URLSearchParams.prototype */
    response_free(g_ctx);
    request_free(g_ctx);   /* Response.prototype — one object, held for the runtime's life */
    idl_args_free(g_ctx);   /* the dictionary member atoms the declaration pool interned */
    navigable_free(g_ctx);
    /* navigator (and Permissions §6 + §3.2's store with it), storage_manager and screen are ROWS on
       core/platform.h's release column now, run by the platform_agent_free above. §3.2's store is two live
       Arrays reached only through navigator_free, and the host that had no such line — the WPT runner —
       leaked both in every file it ran. A teardown each host writes out by hand is a teardown some host is
       missing a row of, and nothing reports it but the runtime's own leak walk, after the fact. */
    location_free();
    session_history_free();
    history_free();
    navigation_free(g_ctx);              /* HTML §7.2.6 the navigation API */
    navigation_history_entry_free(g_ctx);
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
    /* AFTER JS_FreeRuntime, and it is the one teardown line whose ORDER is part of its meaning: what
       this releases is part of a step DEFINITION, which JS_RegisterStepDef borrows and requires to
       outlive the runtime — JS_FreeRuntime's own [stepleak] report reads `def->steps` to name each
       unfinished machine by the step it rests at. */
    idl_async_iter_free();
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
        /* NOBODY IS PARKED ON IT, AND THERE ARE NOW TWO WAYS THAT HAPPENS. The one this asserts against is the
           host naming a URL no flow ever asked for — its pending/provide pairing off by one, so the flow that
           IS parked waits forever. The other is a SALE: the engine hit the RAM floor and paged the flow that
           was waiting on this reply out to the cold tier (engine_take_paged_owed), which is the cheapest member
           there is to page precisely because its recipe re-issues the request next session and gets today's
           answer. A sale is consumed here, one per reply it made unnecessary, so the assert stays exact for the
           case it exists for instead of aborting on the mechanism working. */
        if (n == 0 && !engine_take_paged_owed())
            DFAIL("a reply was provided for a URL no flow is parked on and none was paged out — the host's "
                  "pending/provide pairing is off, and resolving nothing would leave the flow that IS parked "
                  "waiting forever");
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
        engine_host_answer(g_ctx, (uint32_t)req, v, (int)completion, ENGINE_ANSWER_HOST);
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

/* AND THE ONE THAT IS ASKED. `qjs_route` hands this instance a one-way delivery and returns void;
 * `qjs_host_answer` is the trusted zone answering a request it computed ITSELF. Neither of them is a peer
 * PERFORMING an operation, which is the whole of the cross-instance seam's other direction — and its absence is
 * why the shipped engine could EMIT `windowproxy.get` and never be on the receiving end of one.
 *
 * `record` is the operation VERBATIM as the asking instance wrote it (the text after the id on that instance's
 * `qjs_host_requests` line), routed here because this instance holds the document it names. `token` is the
 * ZONE's rendezvous and is opaque to both engines: it says which instance and which request the answer belongs
 * to, which the asking flow's own id cannot, since two peers may hold the same number.
 *
 * NOTHING RUNS INSIDE THIS CALL and there is no answer to read when it returns. A peer answers BY RUNNING A
 * PROGRAM — an IDL getter, a page's setter, a page's function — so the operation becomes a program of every
 * live timeline of the named document, on the ONE frontier, preemptible and parkable at any depth. Each
 * completion leaves through `qjs_host_notices` as `remoteop.answer<TAB><token><TAB><completion>`, and there are
 * as many of them as that document has timelines: its state IS its flows, so `otherW.length` has an answer per
 * timeline and every one of them is true. */
QJS_EXPORT void qjs_perform(const char *token, const char *record)
{
    DCHECK(g_begun, "a cross-agent operation was asked of an engine whose frontier was never seeded — there is "
                    "no timeline to perform it in and no scheduler to run the program that answers it");
    engine_perform(g_ctx, token, record);
}

/* THE PEER'S COMPLETION, COMING BACK — and it crosses in remote_object.c's grammar rather than as JSON, which
 * is why it is a second entry beside qjs_host_answer and not a third argument on it. The two carry different
 * kinds of thing and neither grammar can express the other's: an answer the ZONE computed is a data record
 * (§7.4 step 14's load is `{body, csp}`, which the object-by-NAME grammar cannot say), and an answer a PEER
 * computed may be an OBJECT, which crosses as a name into that peer's namespace and which JSON would have to
 * either serialize — returning something that is not the thing — or drop. The decode happens HERE, inside the
 * asking instance, because a name only means something to an engine: the trusted zone routes the text and
 * never reads it.
 * `req` is this instance's own request id, which the zone recovers from the token it minted. */
QJS_EXPORT void qjs_host_answer_remote(unsigned req, const char *completion)
{
    int type = ENGINE_COMPLETION_NORMAL;
    JSValue v;

    DCHECK(g_begun, "a peer's completion was delivered to an engine that never ran");
    DCHECK(completion != NULL && *completion,
           "a peer's completion arrived with no text — a completion record carries its TYPE in front of its "
           "value, so an empty one is not `undefined`, it is a relay that lost the answer");
    v = remote_completion_decode(g_ctx, completion, &type);
    /* Routed to ONE call site by id, exactly as qjs_host_answer's is: the operation was performed under the
       ASKING FLOW's world, so this answer belongs to that flow and to no other. A zero return means that flow
       is gone, which is not an error — nobody is waiting on the answer. */
    engine_host_answer(g_ctx, (uint32_t)req, v, type, ENGINE_ANSWER_PEER);
    JS_FreeValue(g_ctx, v);
}

QJS_EXPORT double qjs_top_weight(void)
{
    DCHECK(g_begun, "qjs_top_weight was asked of an engine whose frontier was never seeded");
    return engine_top_weight();
}

/* THE VALUE YIELD FLOOR — the runner-up engine's weight, so the running flow yields the moment it is
   outranked ACROSS documents. It was the one entry in this ABI with no contract on it at all, which is how an
   edge stops being an edge: every other entry states what it may be handed and aborts when it is handed
   something else, and this one accepted anything a host could put in a double. A NaN is the case that matters
   and it is silent by construction — every comparison against it is false, so the top flow simply never
   yields and the host's Level-1 interleave stops happening with nothing to say so. */
QJS_EXPORT void qjs_set_yield_floor(double floor)
{
    DCHECK(g_begun, "a yield floor was set on an engine whose frontier was never seeded — there is no flow to "
                    "rank against it");
    DCHECK(!(floor != floor), "the host set a yield floor of NaN — every weight comparison against it is false, "
                              "so the running flow is never outranked and the Level-1 interleave silently stops");
    engine_set_yield_floor(floor);
}

/* RAM PRESSURE, FROM THE ONE ZONE THAT CAN SEE IT. The host holds every live document's engine and the summed
   working set; this instance sees only itself, so the DECISION is the host's and the TIMING is the engine's —
   the park is taken at the next step boundary with no flow switched in, which is the only moment every flow's
   state is a snapshot rather than half of it living in the scheduler. The step that takes it answers DONE and
   the residue leaves in the result document's `_park`, for the host to write to IndexedDB and hand back to
   qjs_begin in a later session. Asking is not parking: the engine keeps running until it reaches that
   boundary, and nothing about the frontier changes in between. */
QJS_EXPORT void qjs_request_park(void)
{
    DCHECK(g_begun, "a park was requested of an engine whose frontier was never seeded — there is nothing to "
                    "write out, and the host would store an empty residue over whatever it had");
    engine_request_park();
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
