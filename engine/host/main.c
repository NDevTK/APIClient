/* The production ABI entry: the `qjs_*` exports `extension/bridge.js` drives, and nothing else. It holds no
 * semantics: it is written against JS_SetTimeTravelHooks, JS_FlowNew/Resume and engine_sched_begin/step;
 * identity is browser/core/loader's Lexbor <script> scan, and the frontier is solver/engine.c's WFQ dispatch loop.
 *
 * The ABI is begin plus step rather than one run because of the cooperative-quantum yield: the host has a message
 * port to pump, other documents' engines to interleave and findings to stream, so the scheduler returns on a
 * bounded slice and resumes the byte-identical frontier.
 *
 * A web-platform edge this engine does not provide is absent rather than stubbed: a page reading it throws its own
 * TypeError, which names the edge to build, where a stub answering undefined would let a flow report a surface it
 * never reached. */
#include <limits.h>   /* UINT_MAX — the pixel run's extent is narrowed to this ABI's own length type */
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
#include "browser/core/url/origin.h"   /* §7.1.1's same origin: the one check that decides what may join this agent */
#include "browser/core/frame/window_proxy.h"
#include "browser/core/frame/remote_location.h"
#include "browser/core/events/broadcast_channel.h"
#include "browser/core/frame/window_message.h"
#include "browser/core/frame/remote_object.h"
#include "browser/core/html/html_iframe.h"
#include "browser/core/loader/document_load_type.h"  /* §7.4.5: which document a response loads as */
#include "browser/core/loader/document_load_decode.h"  /* §13.2.3.2: which decoder that response's bytes need */
#include "browser/core/loader/document_load.h"       /* §7.4.5's load-a-document: the §7.5 subsection it runs */
#include "browser/core/mime/mime_type.h"             /* §7.4.5's computed type is a MIME type record */
#include "browser/core/frame/navigable.h"
#include "browser/core/frame/navigation_params.h"
#include "browser/core/frame/policy_container.h"   /* §7.1.7's determine-navigation-params-policy-container */
#include "browser/core/frame/embedder_policy.h"    /* §7.1.4's item, and §7.1.4.2's check over two of them */
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
#include "browser/core/events/message_port.h"
#include "browser/core/xhr/xml_http_request.h"
#include "browser/core/structured_clone.h"
#include "browser/core/events/event_target.h"
#include "browser/core/platform.h"
#include "browser/core/realm.h"
#include "browser/core/frame/history.h"
#include "browser/core/frame/session_history.h"
#include "browser/core/frame/location.h"
#include "browser/core/frame/navigator.h"
#include "browser/core/frame/screen.h"
#include "browser/core/frame/viewport.h"
#include "browser/core/frame/visual_viewport.h"
#include "browser/core/frame/window.h"
#include "browser/core/rendering/rendering.h"
#include "browser/core/paint/document_paint.h"   /* CSS 2.1 §E.2's ink on CSS 2.1 §2.3.1's region */
#include "browser/core/timing/timer.h"
#include "browser/core/loader/document_scripts.h"
#include "browser/core/dom/node_interface.h"   /* the one place a Document is made; see that header */
#include "solver/absent.h"
#include "solver/concolic.h"
#include "solver/cow.h"
#include "solver/endpoint.h"
#include "solver/engine.h"
#include "solver/flow.h"
#include "solver/quantum.h"
#include "solver/world.h"
#include "solver/result.h"
#include "solver/solve.h"
/* The entries below are declared in qjs_abi.h and defined here, so the compiler checks the two against each
   other, `QJS_EXPORT` marker included. A C caller (`test_forced.c --abi`) uses the same declarations, since
   emscripten's wrapper zero-fills missing arguments, which reach a `const char *` parameter as NULL. */
#include "qjs_abi.h"

/* One WASM instance per origin-keyed agent cluster (SECURITY.md), and these are the root document's: the parse the
 * host handed over, its script inventory, and the one engine that runs the agent's whole frontier. Nothing here is
 * a registry: a same-origin child document is a second realm (engine_child_realm, whose realms the agent owns in
 * navigable.c), and a cross-origin one is a second instance. */
static lxb_html_document_t *g_dom;
static DocScripts           g_scripts;
static unsigned             g_bundle_id;
static JSRuntime           *g_rt;
static JSContext           *g_ctx;
static int                  g_begun;
static int                  g_done;

/* The document this instance last answered with. `result_json` returns a fresh malloc'd document per call
 * (solver/result.h: "caller frees"), and the trusted zone's binding copies the C string and drops the address, so
 * this file frees it: like every other `const char *` entry here, the buffer is the answering side's and the host
 * may read it until it asks again. */
static char                *g_result;

/* The image this instance last rendered, held for `g_result`'s reason. A paint is bytes, not a C string (every
 * byte value occurs in non-premultiplied RGBA; core/graphics/raster_surface.h), so the run and its length are two
 * entries. This file frees it: the previous image before a new render, the last at teardown; a host that kept the
 * address across a second `qjs_paint` would read whatever the next render allocated.
 * `g_painted` is not `g_paint.px != NULL`: a zero-area image (a document CSS 2.1 §2.3.1 "The canvas" establishes no
 * rendered region for) holds NULL pixels and zero bytes as a real answer, while an instance never asked to paint
 * holds the same values and has answered nothing. */
static RasterSurface        g_paint;
static int                  g_painted;

/* What that render did, held beside the image because the image cannot state it. As core/paint/document_paint.h's
 * DCHECK says, "a surface with no ink on it means the walk reached no box, or every box painted nothing, or the
 * walk was stopped — three things a caller must tell apart and a bitmap cannot"; `complete` false is a partial
 * picture, which a host must not publish as the page. The image's shape is read off `g_paint`; this is the
 * provenance of the ink. */
static DocumentPaintCount   g_paint_count;

/* Whose timeline that render was taken in. A document has as many worlds as it has flows (a forked boot flow under
 * `if(__FLAGS.admin)` holds a DOM and heap the primary never held), and `qjs_paint` renders whichever one the heap
 * is standing in, so the name tells two images of two worlds from two reads of one. It is captured at the render,
 * not read live: `qjs_step` switches flows, so a later read would name a timeline other than the one in the
 * bytes. It is this file's allocation, freed on the image's terms. It is composed by solver/world.h's
 * `world_name`, not `world_serialize`, because serializing a world marks it as having crossed to a peer. NULL
 * means no render has been asked for, which `g_painted` also says; it is never the baseline, which has a name. */
static char                *g_paint_world;

/* What that timeline's appearance is worth: whether the path that laid the ink took an arm its own concrete
 * example contradicts (solver/flow.h's `path_forced`; no value can answer that, since the same bytes are the same
 * whichever arm reached them). It is a string literal, so teardown clears it rather than freeing it. It is
 * captured at the render from the same read of the running-flow stamp as `g_paint_world`, so the two describe
 * one world by construction. */
static const char          *g_paint_forced;

/* The documents that joined this agent after it was rooted (`qjs_join`'s realms and trees), held because this
 * host gives them back: `qjs_teardown` frees exactly what `qjs_*` allocated. It answers no question about the
 * agent; the world registry names documents and navigable.c owns the §7.4 child realms. An unfreed joined realm is
 * a leak `JS_FreeRuntime`'s gc_obj_list walk reports, and an unfreed tree outlives its arenas. */
static JSContext          **g_joined_ctx;
static lxb_html_document_t **g_joined_dom;
static int                  g_joined_n;
static int                  g_joined_cap;

/* The agent and the document, split. An agent is a JSRuntime (every class registration, the world registry, the
   flow frontier); a document is a JSContext in it, and a same-origin child navigable is a second document in this
   agent. core/platform.h is the one description of what an agent declares and a document installs, shared by all
   three hosts. What is left here is the host's edges (who answers the network, who evaluates a string handler),
   which each component asserts at its first use. */
static void engine_agent_init(JSContext *ctx, const char *origin, const char *top_level_url, bool requests_oac,
                              OpenerPolicyValue opener_policy)
{
    /* The host's network: SECURITY.md puts every byte behind the trusted chokepoint, so a request parks on the
       flow's pending register and the trusted zone fetches it. fetch.c aborts on a fetch with no provider. */
    { static const FetchProvider P = { engine_pending_fetch_url }; fetch_set_provider(&P); }
    /* Who reads an uncaught page error: `qjs_result` publishes result_json for every page, so `pageErrors` reaches
       the zone whether or not the frontier drained (see solver/result.h). */
    result_page_errors_ride_the_document();

    /* The facts, passed rather than assigned: `requests_oac` (§7.5.1) and `opener_policy` (§7.1.3) are decided by
       the component that reads the response (§7.4.2.1 "Supporting concepts"'s navigation params, which §7.4.5
       "Populating a session history entry" creates by fetching, and which `qjs_init` and `qjs_join` run over the
       zone's header list). core/platform.h says why no host builds the fact struct itself. */
    platform_agent_init(ctx, origin, top_level_url, requests_oac, opener_policy);
}

/* One document: the per-realm half, run once per document including the first. `url` is the document's address
   and `origin` its principal: §8.1.3.2 "Environment settings objects"'s API base URL is the address, which
   `fetch("api/users")` resolves against, so the two are separate arguments. */
static void engine_realm_install(JSContext *ctx, lxb_html_document_t *dom, const char *url, const char *origin,
                                 DocumentKind kind,
                                 SerializedPolicyContainer policy,
                                 SerializedResponsePermissionsPolicy permissions_policy,
                                 SandboxFlags sandbox_flags,
                                 uint32_t doc_id, JSValueConst nav_proxy, const char *about_base_url,
                                 bool is_iframe_srcdoc)
{
    JSValue g = JS_GetGlobalObject(ctx);

    platform_document_install(ctx, g, dom, url, origin, kind, policy, permissions_policy, sandbox_flags,
                              doc_id, nav_proxy, about_base_url, is_iframe_srcdoc);
    JS_FreeValue(ctx, g);
}

/* A same-origin child navigable's realm: a second JSContext in the same JSRuntime, which is what HTML's
   similar-origin window agent is, given the identical per-document install. The address is a fact about the
   document, not a fetch: navigable.c fetches through the document fetcher, and this host declares none, because
   its network parks the flow on a host-owed answer and cannot answer synchronously. navigable.c asserts at the
   address until §7.4's navigate becomes a scheduled work item that parks on the response.
   engine_realm_new is the realm before its document, the one place the per-realm intrinsic list runs for a realm
   after the agent's first. It is separate from the builder below because its two callers differ in whose
   WindowProxy the document is installed with: a §7.3.1.3 "Child navigables" child already has one (its "create a
   new child navigable" minted it), while a joined document is its navigable's first, so window_proxy_new_self
   mints it on this realm. §3.7 gives a realm its own intrinsics, since members run in the realm that defined them.
   `top_level_url` is HTML §8.1.3.1's, decided by the operation: §7.4's creator's for a nested navigable, the
   navigable's own address for an auxiliary one, the zone's statement for a joined document. Passing `url` would
   make an about:blank iframe of an http page a secure context. */
static JSContext *engine_realm_new(JSRuntime *rt, const char *top_level_url)
{
    JSContext *ctx = JS_NewContext(rt);

    /* Named, because the reader of a `@WHY` needs to know what ran out: a realm is materialized per document of
       this agent (each same-origin child navigable and each cross-document navigation of the tab). §7.5.10's
       destruction is a reference drop (core/frame/navigable.c), but this host's `g_joined_ctx`/`g_joined_dom`
       entry is released only at `qjs_teardown`, so the ceiling is ownership, not memory pressure. */
    CHECK(ctx != NULL,
          "a realm of this agent could not be created — an agent materializes ONE REALM PER DOCUMENT (a "
          "same-origin child navigable, and every cross-document navigation of the tab), and this host holds a "
          "reference to each joined one until qjs_teardown, so a long browsing session in one tab accumulates "
          "them whether or not their Documents have been destroyed. BUILD THE HANDOFF: the realm-teardown hook "
          "in core/frame/navigable.c is what releases a realm's Document, and the tree a joined document was "
          "parsed into has to travel with it rather than staying on this host's array");
    CHECK(JS_AddIntrinsicDOMException(ctx) == 0, "the DOMException intrinsic failed to install in a realm");
    /* §3.3.7 [Exposed] step 1's realm side: every realm this host builds is a navigable's, whose global is a
       Window. */
    realm_install_intrinsics(ctx, top_level_url, "Window", false);   /* §8.1.3.5 step 1.2.1: a Window has no owner set */
    return ctx;
}

static JSContext *engine_child_realm(JSRuntime *rt, lxb_html_document_t *dom, const char *url,
                                     const char *top_level_url, const char *origin, DocumentKind kind,
                                     SerializedPolicyContainer policy,
                                     SerializedResponsePermissionsPolicy permissions_policy,
                                     SandboxFlags sandbox_flags,
                                     uint32_t doc_id, JSValueConst nav_proxy, const char *about_base_url,
                                     bool is_iframe_srcdoc)
{
    JSContext *ctx = engine_realm_new(rt, top_level_url);

    engine_realm_install(ctx, dom, url, origin, kind, policy, permissions_policy, sandbox_flags, doc_id,
                         nav_proxy, about_base_url, is_iframe_srcdoc);
    return ctx;
}

/* Parse a document this host was handed: the root's at qjs_init, a joined one at qjs_join. A document of this
   agent is a real Lexbor tree over the bytes the trusted zone captured, and tree construction always produces
   <html><head><body>. A second document parses into the same arenas: core/dom/node_heap.h puts node storage on
   the agent's heap, so DOM §4.5's adopt across two of this agent's documents is a pointer write, which is why
   HTML's similar-origin window agent is the instance boundary.
   The bytes arrive as `(html, html_n)` and a 0x00 is one of them: HTML §13.2.3.5 "Preprocessing the input stream"
   leaves U+0000 to later stages (§13.2.5.1 "Data state" emits it, §13.2.5.4 "Script data state" and §13.2.5.2
   "RCDATA state" emit U+FFFD, §13.2.6.4.7 The "in body" insertion mode ignores it, §13.2.6.5 "The rules for parsing
   tokens in foreign content" inserts U+FFFD), so the length is never taken with `strlen`.
   It asks which kind of document the response loads as (§7.4.5's load-a-document dispatches on the computed type,
   from the response's header list) and which encoding it is in (§13.2.3.2 "Determining the character encoding"),
   through core/loader's classification, decoder and §7.5 subsection, the same ones the child loader uses.
   `out_kind` (DOM §4.5's type and content type) and `out_encoding` (what DOM §4.5 "Interface Document"'s
   `characterSet` reports) are handed back for the install, because the response is here and the realm is built
   after this returns. */
static lxb_html_document_t *engine_parse_document(const char *html, size_t html_n,
                                                  const HeaderList *response_headers, DocumentKind *out_kind,
                                                  int *out_encoding)
{
    lxb_html_document_t *dom;
    MimeType computed;
    lxb_status_t st;
    char *decoded;
    size_t decoded_n;
    int encoding;

    /* No default for a missing document: an empty-page analysis reads exactly like a page with no endpoints and
       no sinks. Both entries CHECK the pointer, so this asserts. A zero length is legitimate: an empty response
       is still a Document, and tree construction still produces <html><head><body>. */
    DCHECK(html != NULL,
           "a document was parsed from no bytes at all — both entries CHECK the pointer before reaching here, "
           "so a null one is a third caller that does not");
    /* §7.4.5's first step, "Let type be the computed type of navigationParams's response", asked where the
       response is and carried to the parse with the bytes it describes. */
    DCHECK(response_headers != NULL,
           "a Document was built out of a response with no header list — MIME Sniffing §7's computed type is "
           "what HTML §7.4.5 dispatches on, and a caller with bytes and no headers is one that never asked "
           "which kind of document it fetched");
    document_load_computed_type(&computed, response_headers, html, html_n);
    /* §13.2.3.2 over the same response, and the decode its answer names. The type is computed from `html`, not
       `decoded`, because MIME Sniffing §5.2 "Reading the resource header" reads the resource's own bytes. No
       parent encoding: §13.2.3.2's container-document step is for a document whose navigable has a parent, and
       this is the instance's root; the child navigable's entry passes its container's. */
    encoding = document_load_decode(&decoded, &decoded_n, response_headers, html, html_n,
                                    /*parent_encoding*/-1);
    dom = dom_document_create();

    CHECK(dom != NULL, "the document allocation failed");
    /* Shared: this is the active document, whose tree is the baseline every flow reads, so §13.2.6's writes are
       the ones a live parse captures, which makes §8.4.3 `document.write()` a lifecycle change (solver/dom_cow.h).
       Which parser runs is §7.4.5's arm, routed by core/loader/document_load.h, which asserts the HTML parser is
       unreachable for a type it does not serve; the CHECK_FAIL below is this host's release half of an unbuilt arm.
       §13.2.4.5's scripting flag is set: the boot flow drives this document's scripts, and the navigable and realm
       come after this returns (core/html/html_parse.h says why the flag is a parameter). The parser is given the
       decode, never the response's bytes (§13.2.3.1 "Parsing with a known character encoding"), with the
       encoding beside it so the loader can refuse a caller that determined none. */
    st = document_load(dom, DOM_PARSE_ROOT_SHARED, HTML_SCRIPTING_ENABLED, &computed, encoding,
                       (const lxb_char_t *)decoded, decoded_n);
    /* Before the record is freed, because the pair is read out of it. */
    DCHECK(out_kind != NULL, "a document was parsed with nowhere to put its §7.5.1 type and content type — "
                             "the Document those two facts belong to is installed by this host and there is "
                             "exactly one caller shape, so a null here is an entry that will install a "
                             "Document without knowing what was fetched");
    DCHECK(out_encoding != NULL,
           "a document was parsed with nowhere to put HTML §13.2.3.2 Determining the character encoding's "
           "answer — the Document it belongs to is installed by this host after this returns, so a null here "
           "is an entry that will install a Document reporting DOM §4.5's utf-8 default for a response that "
           "was decoded as something else");
    *out_kind     = document_load_kind(&computed);
    *out_encoding = encoding;
    mime_type_free(&computed);
    /* The decode is borrowed for the life of the load (core/loader/document_load.h), and this load is complete,
       so these bytes are this frame's to release. */
    free(decoded);
    if (st != LXB_STATUS_OK)
        CHECK_FAIL("the document parse failed — the DOM is the ground truth every flow reads");
    return dom;
}

/* Phase 1: parse and identify. No script runs: the host reads the frontier key back synchronously and looks up
 * the prior session before any flow is seeded.
 * `doc_id` names this instance's document; the host names the root one because only it knows there is more than
 * one. A cross-origin iframe or popup is a peer instance that keys its segment of the flow's world by this name,
 * so two instances sharing a name would be one timeline wearing two names. Documents below this one are named by
 * their creator (world.h), so §4.8.5 can create a child navigable without asking.
 * There is no script-text argument: identity and the script inventory come from the DOM's own `<script>` scan,
 * since a concatenation cannot represent per-script scope.
 * `url` is the document's address, and the origin is derived from it here (§4.7's serialization in url.c), so
 * §8.1.3.2's API base URL, `location.pathname` and `document.baseURI` see the address.
 * `top_level_url` is HTML §8.1.3.1's top-level creation URL, which only the trusted zone can state: this
 * instance's document may be nested in another instance's, and §8.1.3.5 decides a secure context from the top of
 * that chain (Secure Contexts §4.2's ancestral rule). A top-level document passes its own address; a document a
 * peer created passes the field that peer put on its `navigable.create` notice. */
/* HTML §7.1.7's clone of the creator's container as it arrives over this ABI, read in one place for both entries
 * (`qjs_init` and `qjs_join` take the identical contract). The values arrive as §7.1.4's own tokens, relayed
 * verbatim by the zone from the emitting engine's `navigable.create` notice; a token naming none of the three
 * strings is a relay that stopped writing the field (not §7.1.4.1's fail-open, which is about a server header),
 * and it crashes at that boundary. */
static SerializedPolicyContainer qjs_inherited_container(const char *csp, const char *self_origin,
                                                         const char *coep, const char *coep_endpoint,
                                                         const char *coep_report_only,
                                                         const char *coep_report_only_endpoint)
{
    EmbedderPolicyValue v = EMBEDDER_POLICY_UNSAFE_NONE, ro = EMBEDDER_POLICY_UNSAFE_NONE;
    /* The parse runs, then the assert reads its answer: a DCHECK condition is compiled out in release, so a parse
       inside one would not happen in the shipped build. */
    bool ok = embedder_policy_value_of_token(coep, &v);
    bool ok_ro = embedder_policy_value_of_token(coep_report_only, &ro);

    (void)ok; (void)ok_ro;
    DCHECK(coep_endpoint != NULL && coep_report_only_endpoint != NULL,
           "a §7.1.7 policy container arrived over this ABI with no §7.1.4 REPORTING ENDPOINT — the section "
           "makes both of them strings whose absence is the EMPTY one, so a NULL is the relaying zone having "
           "stopped writing the field rather than a policy that names no endpoint");
    DCHECK(ok && ok_ro,
           "a §7.1.7 policy container arrived over this ABI with an §7.1.4 EMBEDDER POLICY VALUE that names "
           "none of the three strings the section defines — the emitting engine writes these tokens onto its "
           "`navigable.create` notice and the trusted zone relays them verbatim, so a token outside them is a "
           "relay that stopped writing the field, not a header that failed to parse. Read as `unsafe-none` it "
           "would put a cross-origin isolated creator's child under the wrong rule for every no-CORS fetch");
    return serialized_policy_container_or_none(csp, self_origin,
                                               serialized_embedder_policy(v, coep_endpoint, ro,
                                                                          coep_report_only_endpoint),
                                               /* Named residual: §7.1.7's integrity policy item is not on this
                                                  ABI entry. The in-instance carriers (the navigate job's
                                                  arguments and the parked navigable record) carry it; this is a
                                                  container a peer instance stated, relayed by the zone.
                                                  Not covered: a document whose creator lives in another
                                                  instance inherits its CSP but not its integrity policy.
                                                  Next diff builds: the field on the `navigable.create` notice,
                                                  written where the emitting engine serializes its container
                                                  and read here. How its absence shows: such a child loads a
                                                  `<script src>` without `integrity` that its creator's policy
                                                  refuses, while a same-instance sibling is refused it. */
                                               NULL,
                                               /* Named residual: §7.1.7's referrer policy item is not on this
                                                  entry either. Not covered: a child whose creator lives in
                                                  another instance holds the default referrer policy. Next diff
                                                  builds: the field on the same `navigable.create` notice. How
                                                  its absence shows: such a child's requests carry a referrer its
                                                  creator's policy strips, while a same-instance sibling's do
                                                  not. */
                                               NULL);
}

/* `headers` is the navigation response's header list, as HTTP field lines (`name: value`, one per line), which
 * core/fetch/headers.c parses into Fetch's list. §7.1.7's embedder policy, §7.5.1's opener policy row and §7.5.1's
 * `Origin-Agent-Cluster` read (§8.1.2.2's agent cluster key) all read names out of one list, and Fetch's `get`
 * decides what a repeated header means (§7.1.4.1 turns on `require-corp, require-corp` failing to parse), so it
 * is a list of lines, not a map. An empty or NULL block is a response with no headers. The engine cannot forge
 * these: no ABI entry or page-reachable member writes a response header (a `<meta>` carries only a CSP, CSP §3.3,
 * merged by core/dom/document.c), and the list is read once, here, then freed.
 *
 * `html_len` is the document's length, an argument because a document may contain a 0x00 (§13.2.3.5 defines its
 * handling); `Init`'s wire parameter is mojom's `array<uint8>`. `unsigned` because every length crosses this ABI
 * at that width (`qjs_provide`'s `body_len`) and the wire declares an i32. renderer.html writes a guard NUL after
 * the document; it is asserted, not used as the length, so a placement or length disagreement crashes here rather
 * than inside lexbor. */
/* `inherited_csp` and `inherited_csp_self_origin` are the creator's policy container, beside `headers` because
 * they are not part of it: a document this instance did not root (a cross-origin child navigable announced by a
 * `navigable.create` notice) is created with HTML §7.1.7 "Policy containers"' clone of its creator's container
 * (§7.3.2.1 "Creating browsing contexts": "Set document's policy container to a clone of creator's policy
 * container"). CSP §2.2 "Policies" makes a CSP list "a struct consisting of policies (a list of policies) and a
 * self-origin", and §2.2.2 sets a header policy's self-origin to "response's URL's origin", so an inherited list
 * relayed as a header would resolve `'self'` (§6.7.2.8) against the child's origin. The empty string in both means
 * no creator; a self-origin with an empty policy is a creator with no policies; a policy without a self-origin
 * cannot exist and is asserted. Which container the document is created with is §7.1.7's
 * determine-navigation-params-policy-container (core/frame/policy_container.h), called with both in hand.
 * The four `inherited_coep*` arguments are the same container's §7.1.4 item, since a clone copies every item;
 * without them a cross-origin-isolated creator's child would silently be `unsafe-none`. They are read through
 * qjs_inherited_container. */
/* `parent_navigable` is §7.3.1.3 "Child navigables"' parent of this document's navigable, stated by the zone that
 * routed the create, since a cross-origin `<iframe>` is the root of its own instance (navigable_root in
 * core/frame/navigable.h). `container_policy` is the creator's result of Permissions Policy §9.5 ("given null or
 * an element (container) and an origin (origin)"), whose arguments are both the creator's, in the text
 * core/permissions_policy/permissions_policy.h serializes; "there is no container" is that grammar's word for an
 * auxiliary navigable or a root with no embedder. Without it Permissions Policy §9.7 step 1 would grant every
 * feature, so core/dom/document.c crashes on its absence.
 * `ancestor_origins` is HTML §3.1.3 "Ancestor origins"' internal ancestor origin objects list, computed by the
 * creating instance, where its three inputs are: the parent document's recorded list (step 5), its origin record
 * (steps 9 and 10) and the container element (step 6). An element and an origin record cannot cross (every opaque
 * origin serializes to `null`, and step 10 compares by identity), so this is the result, in core/dom/document.h's
 * grammar; "there are no ancestors" is a top-level or auxiliary document's statement. core/dom/document.c crashes
 * on its absence, since an empty list would claim this document is at the top. */
/* `creation_sandbox_flags` is HTML §7.1.5 "Sandboxing"'s creation sandboxing flag set for this document's
 * navigable, separate from the container (§7.3.2.1 sets them in different steps). §7.1.5 reads the embedder's
 * iframe sandboxing flag set and its node document's active sandboxing flag set, which live in the creating
 * instance, so this is the result, in §7.1.5's own flag names (core/frame/sandboxing.h), `none` for the empty set.
 * It is the sandboxing flags of every §7.4.2.1 target snapshot params this root's navigations take, and §7.4.2.2
 * unions it with the response's CSP-derived flags (navigation_params computes those). A cross-origin
 * `<iframe sandbox>` child is in another agent cluster because the sandboxed origin flag made its origin opaque,
 * so the sandboxed case is the ordinary one on this route. */
QJS_EXPORT int qjs_init(const char *html, unsigned html_len, const char *url, const char *doc_id,
                        const char *headers, const char *top_level_url,
                        const char *inherited_csp, const char *inherited_csp_self_origin,
                        const char *inherited_coep, const char *inherited_coep_endpoint,
                        const char *inherited_coep_report_only,
                        const char *inherited_coep_report_only_endpoint,
                        const char *parent_navigable, const char *container_policy,
                        const char *ancestor_origins, const char *creation_sandbox_flags)
{
    /* §7.1.5's set, parsed before anything is built from it; see the read below. */
    SandboxFlags creation_flags = 0;
    char *origin;
    HeaderList response_headers;
    /* §7.5.1's type and content type for the Document this entry installs, decided by the parse's §7.4.5 dispatch
       and carried to the install. */
    DocumentKind      root_kind;
    int               root_encoding;
    NavigationParams np;

    /* This entry roots the agent at one document. An instance is an origin-keyed agent cluster with one realm per
       same-origin document (engine_child_realm), so what may not happen twice is the rooting: the runtime, class
       registrations, world registry and frontier exist once. A second document of the cluster joins through
       `qjs_join`; calling this twice would silently be a second agent. */
    CHECK(g_dom == NULL, "qjs_init ran twice in one WASM instance — it ROOTS the agent, and a second document "
                         "of this origin-keyed cluster joins through the join entry rather than re-rooting it");
    CHECK(html != NULL, "the host started this engine with no document at all — a Document is a tree over the "
                        "bytes the trusted zone captured, and there is no tree to build from nothing");
    /* The length and the guard are one fact both sides state: renderer.html writes a NUL after the sequence, and
       this reads that byte and nothing else past `html_len`. */
    DCHECK(html[html_len] == '\0',
           "a document's bytes carry no guard byte at their stated length — the ABI placement writes one past "
           "the sequence, so a length that does not name that byte is this entry and the trusted zone holding "
           "two different documents under one call");
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

    /* The response, read once before anything is built from it. §7.4.2.1 "Supporting concepts"'s navigation
       params, created by §7.4.5 "Populating a session history entry"'s "create navigation params by fetching", are
       what §7.5.1 "Shared document creation infrastructure" creates a Document from, decided at the response and
       carried, never read back off the Document. The secure-context answer is §8.1.3.5's over the top-level
       creation URL, asked before the realm exists because §7.1.3's and §7.1.4's obtains and §7.5.1's `requestsOAC`
       run against the reserved environment. The target snapshot params' sandboxing flags are the caller's
       statement for both navigables this entry roots: a top-level traversable states `none` (§7.1.5 answers it
       from an empty popup sandboxing flag set), and a peer-created navigable states its embedder's set;
       `parent_navigable` says which this is. The response's CSP-derived half is navigation_params'. Parsed, then
       asserted, as in qjs_inherited_container. */
    {
        bool ok = sandbox_flags_of_serialized(creation_sandbox_flags, &creation_flags);

        (void)ok;
        DCHECK(ok,
               "a document arrived over this ABI with no readable §7.1.5 CREATION SANDBOXING FLAG SET — the "
               "creating instance writes that section's own flag names comma-separated onto its "
               "`navigable.create` notice, and `none` for the empty set, so a field that is empty or carries a "
               "word outside those names is a relay that stopped writing it rather than a navigable with no "
               "flags. Read as the empty set it would create a cross-origin `<iframe sandbox>` child that runs "
               "the scripts, submits the forms and relaxes the `document.domain` its embedder's markup forbids");
    }
    /* §7.3.2.1's determine the origin returns a new opaque origin when the sandboxed origin browsing context flag
       is set, so a navigable whose creation set carries that flag has an opaque principal,
       while this instance's is the tuple origin the zone derived from the fetched address. It is asserted, not
       corrected here: SECURITY.md makes the principal the trusted zone's to state, and an engine demoting itself
       would disagree with the SOP answers `safeFetch` enforces for it.
       Named residual. Not covered: a sandboxed cross-origin frame runs under its server's real origin, same origin
       with its siblings from that host. Next diff builds: the zone mints a per-document opaque token when the
       create notice announces this flag (extension/bridge.js's `_senderOrigin` shape, engine/trusted.mjs). How its
       absence shows: this DCHECK fires on such a frame. */
    DCHECK(!(creation_flags & SANDBOX_ORIGIN) || !strcmp(origin, "null"),
           "a document was rooted with §7.1.5's SANDBOXED ORIGIN flag set and a TUPLE origin for a principal — "
           "§7.3.2.1's determine the origin returns a NEW OPAQUE ORIGIN for that flag, so this instance is "
           "running a sandboxed Document as same origin with every other document of its server. The trusted "
           "zone derives this principal from the URL it fetched and must mint a per-document opaque token when "
           "the create notice announces this flag (extension/bridge.js, engine/trusted.mjs)");
    memset(&response_headers, 0, sizeof response_headers);
    header_list_parse_field_lines(&response_headers, headers);
    navigation_params_from_response(&np, &response_headers, creation_flags,
                                    secure_context_url_potentially_trustworthy(top_level_url));
    /* The list is not freed here: §7.4.5 reads this response twice, for the navigation params above and for the
       computed type that decides which kind of Document its bytes load as, at the parse below. */

    g_rt = JS_NewRuntime();
    CHECK(g_rt != NULL, "the runtime allocation failed: a dropped engine loses the whole frontier");
    JS_SetMaxStackSize(g_rt, 4 * 1024 * 1024);   /* align quickjs's overflow check with the 8MB wasm stack */
    g_ctx = JS_NewContext(g_rt);
    CHECK(g_ctx != NULL, "the context allocation failed");
    /* DOMException is a Web IDL interface the DOM specs throw by name (NotFoundError, NamespaceError,
       InvalidCharacterError), and the fork implements the whole intrinsic. */
    CHECK(JS_AddIntrinsicDOMException(g_ctx) == 0, "the DOMException intrinsic failed to install");

    concolic_init(g_ctx);
    /* Not a default: an instance that does not know which document it is cannot be reached by a peer, and a
       guessed name would collide with every other instance that guessed the same. */
    DCHECK(doc_id != NULL && *doc_id, "the host started this engine without a document name — an agent is named by its root "
                     "peer instance names this document's flows by that id when it materializes their COW "
                     "segments, so an unnamed document cannot take part in cross-document time travel");
    flow_registry_init(doc_id);
    endpoint_init();
    solve_init(g_ctx);

    /* The two hook sets the solver owns, each declared by its own component and shared with test_forced.c. */
    cow_install_time_travel_hooks(engine_gen_fork);
    concolic_install_hooks();
    concolic_install_source_overlay();   /* a solver host: attacker-controlled values are symbolic sources */

    g_dom = engine_parse_document(html, html_len, &response_headers, &root_kind, &root_encoding);
    header_list_free(&response_headers);

    /* Identity and script inventory from the DOM's own executable scripts: a concatenation of them cannot
       represent per-<script> scope and would shift with an inline script the page did not ship. */
    g_bundle_id = document_bundle_id(g_dom);
    g_scripts   = document_exec_scripts(g_dom);

    /* The web-platform surface, installed on the baseline before any flow runs, so these globals are pre-flow
       state and never land in a delta. What is not built is absent, and the page's own throw names it. */
    engine_agent_init(g_ctx, origin, top_level_url, np.requests_oac, np.opener.value);
    /* §7.4 calls back here for a same-origin child: a same-origin document is a second realm in this heap (HTML's
       similar-origin window agent), and what a document's platform surface is in this build is this file's
       answer. */
    navigable_set_realm_builder(engine_child_realm);
    /* The root navigable is this host's, so its §7.2.3 proxy is minted here, by whoever owns the navigable. */
    {
        /* NULL: this host does not know the navigable's name. A cross-origin opener may have set `name` before
           navigating, which is how window.name is an attacker source, so the read is concolic until stated. */
        /* Which CSP list this document is created with, and CSP §2.2's self-origin of it: one answer from
           §7.1.7's determine-navigation-params-policy-container (core/frame/policy_container.h), shared with the
           other program entry. `origin` is §2.2.2's answer ("response's URL's origin"); the inherited pair is the
           clone of the creator's container relayed off the `navigable.create` notice. The self-origin is its own
           argument because a principal and the origin `'self'` resolves against are different facts, which an
           inherited container makes disagree. It is computed before the mint because §7.1.4.2's step 2 reads its
           embedder policy, and a check after the Document exists could not block it. */
        SerializedPolicyContainer inherited =
            qjs_inherited_container(inherited_csp, inherited_csp_self_origin, inherited_coep,
                                    inherited_coep_endpoint, inherited_coep_report_only,
                                    inherited_coep_report_only_endpoint);
        /* §7.5.1's opener policy row for the root Document: §7.1.3's policy obtained from this instance's
           response, the navigable's because §7.1.3.2 and §7.3.2.1 read it off a browsing context's active
           document (core/frame/window_proxy.h). §7.3.1.3's parent, stated by the zone that routed the create,
           decides whether this root is a top-level traversable; navigable_root runs §7.1.4.2 over it. */
        JSValue root_proxy = navigable_root(g_ctx, world_local_doc(), NULL, np.opener.value,
                                            parent_navigable, inherited, &np.embedder, creation_flags);
        CHECK(!JS_IsException(root_proxy), "the root navigable's WindowProxy could not be allocated");
        /* §7.3.1.3's other link, before the document below is installed: Permissions Policy §9.5 runs once per
           Document a navigable is given, and for a navigable rooted in a peer's frame tree only the creating
           instance can answer it. §7.3.1.3 creates a navigable and links it afterwards. */
        navigable_root_container(g_ctx, root_proxy, container_policy);
        /* §3.1.3's ancestor origins for that navigable's document, before the install, since §7.3.2.1 reads it
           there (see core/frame/navigable.h). */
        navigable_root_ancestor_origins(g_ctx, root_proxy, ancestor_origins);
        /* §7.5.1's Document, from the navigation params decided above: §7.4.5's final sandboxing flag set, §7.1.7
           step 3's CSP list and §7.1.7's create-a-policy-container-from-a-fetch-response step 4 embedder item are
           `np`'s (core/frame/navigation_params.c); this file says which navigable and which realm. */
        SerializedPolicyContainer response =
            serialized_policy_container(np.csp, origin, serialized_embedder_policy_of(&np.embedder),
                                        np.integrity_policy, np.referrer_policy);
        SerializedPolicyContainer policy =
            policy_container_determine_navigation_params(url, response, inherited);

        engine_realm_install(g_ctx, g_dom, url, origin, root_kind, policy,
                             serialized_response_permissions_policy(np.permissions_policy,
                                                                    np.permissions_policy_report_only),
                             np.sandbox_flags,
                             /* §7.4's about base URL: NULL. This root document is created from a response, the
                                arm §2.4.3 "Document base URLs" answers with the document's own address. */
                             world_local_doc(), root_proxy, /*about_base_url*/ NULL,
                             /*is_iframe_srcdoc*/ false);
        /* HTML §13.2.3.2 "Determining the character encoding"'s answer onto the Document, as
           core/frame/navigable.c does: the Document record is the realm builder's product, so this is the first
           moment there is anything to write it on. Without it DOM §4.5 "Interface Document"'s UTF-8 default
           stands for every response. */
        document_set_encoding(g_ctx, root_encoding);
        JS_FreeValue(g_ctx, root_proxy);
    }
    /* The surface is installed, so every member the platform has is declared; a declaration from here on is a
       per-wrapper or per-flow mint, which the pool asserts against. */
    idl_args_seal();

    /* The response is no longer reachable: its decisions are on the Document and the agent's cluster, and nothing
       the bundle runs can ask for a header again. */
    navigation_params_free(&np);
    free(origin);
    return 0;
}

/* A second document of this agent cluster, joined to the instance already running it. `qjs_init` roots the agent
 * (runtime, class registrations, origin, world registry, frontier) once; every other document of this
 * `(browsing-context group, origin)` cluster that this agent did not create arrives here: a same-origin frame the
 * engine never modelled, a second cross-origin child of a peer's page at this origin, a navigation replacing the
 * group's top. HTML puts a similar-origin window agent in one heap and relies on it (§4.5's adopt moves a live
 * node across; `frame.contentWindow.onunload = fn` hands a live closure across).
 * The parameters are the operation's, each meaning what `qjs_init`'s same-named argument means, read once into the
 * decisions §7.5.1 creates a Document from; re-deriving one from the target would join the root document a second
 * time. The target decides its principal, heap, frontier and identity: `qjs_bundle_id` is the agent's frontier
 * key, untouched here, while `doc_id` names the document a peer routes on.
 * One instance per (browsing-context group, origin) is a CHECK: SECURITY.md makes the instance the principal, so a
 * foreign-origin document would be two principals behind one `pageOrigin`. It fails closed on an opaque origin on
 * either side (§7.1.1). The document is built at the baseline, as the root's is; its scripts are flows on the one
 * frontier (engine_join_document). A same-origin document nested through a cross-origin one is a child navigable
 * whose parent, container and ancestors live in a peer, which is why those arguments are carried here too. */
QJS_EXPORT int qjs_join(const char *html, unsigned html_len, const char *url, const char *doc_id,
                        const char *headers, const char *top_level_url,
                        const char *inherited_csp, const char *inherited_csp_self_origin,
                        const char *inherited_coep, const char *inherited_coep_endpoint,
                        const char *inherited_coep_report_only,
                        const char *inherited_coep_report_only_endpoint,
                        const char *parent_navigable, const char *container_policy,
                        const char *ancestor_origins, const char *creation_sandbox_flags)
{
    /* §7.1.5's set for this document's navigable, stated as in qjs_init. */
    SandboxFlags creation_flags = 0;
    HeaderList response_headers;
    /* §7.5.1's type and content type for the Document this entry installs, decided by the parse's §7.4.5 dispatch
       and carried to the install. */
    DocumentKind      joined_kind;
    int               joined_encoding;
    NavigationParams np;
    lxb_html_document_t *dom;
    JSContext *cctx;
    DocScripts scripts;
    uint32_t doc;

    /* A join needs an agent to join (qjs_init) and a frontier for this document's scripts (qjs_begin). A host may
       hold the instance between the two (it reads the bundle id there); a document joined then would never run a
       line and would read as one with no scripts. */
    DCHECK(g_dom != NULL,
           "qjs_join ran on an instance qjs_init never rooted — a join ADDS a document to a LIVE agent, so "
           "there is no runtime to build its realm in, no origin to check it against and no heap for its tree");
    DCHECK(g_begun,
           "qjs_join ran before qjs_begin seeded the frontier — a joined document's scripts are members of the "
           "ONE frontier, and there is no session for them to be members of yet");
    CHECK(html != NULL, "a document joined this agent with no bytes at all — a join ADDS a Document, and there "
                        "is no tree to build from nothing");
    /* The same two-sided read `qjs_init` makes. */
    DCHECK(html[html_len] == '\0',
           "a joined document's bytes carry no guard byte at their stated length — the ABI placement writes "
           "one past the sequence so that a length and a C read cannot disagree silently");

    /* The principal, before anything is built from the bytes: §7.1.1's same origin of the agent's record against
       this document's address, asked first because the parse into the agent's arenas, the realm and the flow
       cannot be undone once done. */
    {
        UrlRecord rec;

        CHECK(url != NULL && *url && url_parse(&rec, url, strlen(url), NULL),
              "a document was joined to this agent without an ADDRESS — a document is loaded FROM somewhere, "
              "and every relative URL it builds resolves against it");
        CHECK(origin_same_as_url(origin_agent(), &rec),
              "a document whose origin is NOT this agent's principal was joined to it — an instance is an "
              "ORIGIN-KEYED AGENT CLUSTER and SECURITY.md makes the instance the principal, so this would put "
              "TWO origins behind one credentialed-read principal and behind one `pageOrigin`. A cross-origin "
              "document is a SECOND INSTANCE the trusted zone provisions, never a second realm in this heap");
        url_record_free(&rec);
    }
    CHECK(top_level_url != NULL && *top_level_url,
          "a document was joined with no TOP-LEVEL CREATION URL — HTML §8.1.3.5 reads it to decide whether "
          "this document's realm is a secure context, and Web IDL §3.3.13's members exist or do not by that "
          "answer, so the platform surface this document's bundle runs against would be a guess. Only the "
          "trusted zone knows what embeds a document of this instance");
    DCHECK(doc_id != NULL && *doc_id,
           "a document was joined with no NAME — a peer routes a delivery and a cross-agent operation on that "
           "name, and every flow of this document mints its worlds under it, so an unnamed document can take "
           "no part in cross-document time travel");

    /* §7.4.2.1 "Supporting concepts"'s target snapshot params' sandboxing flags for the joined navigable, which the
       zone states. §7.1.5's sandboxed origin flag cannot be present (it forces an opaque origin, which the
       principal CHECK above refuses), and that is asserted; the rest of the set can be:
       `<iframe sandbox="allow-same-origin allow-scripts" src="https://cdn/…">` in a page at another origin leaves
       fourteen flags set and puts the child in the cdn's cluster. Parsed, then asserted, as in
       qjs_inherited_container. */
    {
        bool ok = sandbox_flags_of_serialized(creation_sandbox_flags, &creation_flags);

        (void)ok;
        DCHECK(ok,
               "a document joined this agent with no readable §7.1.5 CREATION SANDBOXING FLAG SET — the "
               "creating instance writes that section's own flag names comma-separated onto its "
               "`navigable.create` notice, and `none` for the empty set, so a field that is empty or carries a "
               "word outside those names is a relay that stopped writing it rather than a navigable with no "
               "flags. Read as the empty set it would join a sandboxed frame's document with its sandbox "
               "deleted — the forms, the modals and the downloads its embedder's markup forbids");
        DCHECK(!(creation_flags & SANDBOX_ORIGIN),
               "a document carrying §7.1.5's SANDBOXED ORIGIN flag was JOINED to this agent — that flag makes "
               "§7.3.2.1's determine the origin mint a fresh OPAQUE origin, which is same origin with nothing, "
               "so such a document shares an agent cluster with NO other document and can never be a second "
               "realm in this heap. The principal CHECK above should have refused it first, so reaching this "
               "line means the trusted zone routed a join for a cluster key it derived without the flag set it "
               "had just been told");
    }
    memset(&response_headers, 0, sizeof response_headers);
    header_list_parse_field_lines(&response_headers, headers);
    navigation_params_from_response(&np, &response_headers, creation_flags,
                                    secure_context_url_potentially_trustworthy(top_level_url));
    /* Held past the parse, for qjs_init's reason: §7.4.5 reads this response for its navigation params and for
       the computed type the parse dispatches on. */

    /* This agent holds it, said before the realm is built: §7.2.3's mint below asserts it, and it makes every
       same-origin read through this document answer in this turn rather than suspend on a nonexistent peer. */
    doc = world_doc_intern(doc_id);
    DCHECK(!world_doc_hosted(doc),
           "the document this join names is one this agent ALREADY holds — a document is joined once, and "
           "building a second realm for a name that already answers with one would leave every peer's route "
           "to it resolving to whichever of the two the registry wrote last");
    world_doc_adopt(doc);

    dom = engine_parse_document(html, html_len, &response_headers, &joined_kind, &joined_encoding);
    header_list_free(&response_headers);
    cctx = engine_realm_new(g_rt, top_level_url);
    {
        /* NULL: this host does not know the navigable's name. Whoever embeds this document (a peer or the browser)
           created the navigable, and a peer's `navigable.create` notice carries no name, so `window.name` reads
           concolic until stated; `window_proxy_new_self` spells that with a NULL, which §7.4's mint cannot.
           The principal is the agent's own record, which the CHECK above made the right answer. CSP §2.2's
           self-origin is resolved with the list it belongs to, since a document reaching this entry is one another
           instance announced, so an inherited container is the ordinary case. It is computed before the mint, for
           qjs_init's reason (§7.1.4.2 step 2 reads its embedder policy). */
        SerializedPolicyContainer inherited =
            qjs_inherited_container(inherited_csp, inherited_csp_self_origin, inherited_coep,
                                    inherited_coep_endpoint, inherited_coep_report_only,
                                    inherited_coep_report_only_endpoint);
        /* §7.5.1's opener policy row for the joined Document, from its own response's header list, and §7.3.1.3's
           parent: a same-origin document nested through a cross-origin one belongs to this cluster and to another
           instance's frame tree at once. */
        JSValue proxy = navigable_root(cctx, doc, NULL, np.opener.value, parent_navigable, inherited,
                                       &np.embedder, creation_flags);

        CHECK(!JS_IsException(proxy), "the joined navigable's WindowProxy could not be allocated");
        /* §7.3.1.3's container, for qjs_init's reason: a document nested through a cross-origin one has both of
           its §7.3.1.3 links in a peer instance. */
        navigable_root_container(cctx, proxy, container_policy);
        /* §3.1.3's ancestor origins for that navigable's document, composed in the peer that holds them. */
        navigable_root_ancestor_origins(cctx, proxy, ancestor_origins);
        SerializedPolicyContainer response =
            serialized_policy_container(np.csp, origin_serialized(origin_agent()),
                                        serialized_embedder_policy_of(&np.embedder), np.integrity_policy,
                                        np.referrer_policy);
        SerializedPolicyContainer policy =
            policy_container_determine_navigation_params(url, response, inherited);

        engine_realm_install(cctx, dom, url, origin_serialized(origin_agent()), joined_kind, policy,
                             serialized_response_permissions_policy(np.permissions_policy,
                                                                    np.permissions_policy_report_only),
                             np.sandbox_flags, doc, proxy,
                             /* a joined document is a second response of this cluster; see the root's install
                                for why §7.4's about base URL is null */
                             /*about_base_url*/ NULL, /*is_iframe_srcdoc*/ false);
        /* §13.2.3.2's answer onto this document, decoded with its own encoding, for qjs_init's reason. */
        document_set_encoding(cctx, joined_encoding);
        JS_FreeValue(cctx, proxy);
    }

    if (g_joined_n == g_joined_cap) {
        int cap = g_joined_cap ? g_joined_cap * 2 : 4;
        JSContext **c = realloc(g_joined_ctx, (size_t)cap * sizeof *c);
        lxb_html_document_t **d = realloc(g_joined_dom, (size_t)cap * sizeof *d);

        CHECK(c != NULL && d != NULL, "the host ran out of memory recording a joined document — an unrecorded "
                                      "one is a realm and a tree nothing gives back at teardown");
        g_joined_ctx = c;
        g_joined_dom = d;
        g_joined_cap = cap;
    }
    g_joined_ctx[g_joined_n] = cctx;
    g_joined_dom[g_joined_n] = dom;
    g_joined_n++;

    /* The document runs its own scripts: a Document given a realm, a tree and a WindowProxy but never seeded
       contributes no endpoints. The inventory is the DOM's own `<script>` scan, freed here, because
       engine_join_document copies every body it queues and resolves every `src` it parks on. */
    scripts = document_exec_scripts(dom);
    engine_join_document(cctx, doc, scripts.bodies, scripts.srcs, scripts.types, scripts.sched, scripts.els,
                         scripts.n);
    doc_scripts_free(&scripts);

    navigation_params_free(&np);
    return 0;
}

/* The document a navigation replaced: the other half of `qjs_join`, one navigation. Only the trusted zone can state
 * it: a navigation the real browser performed leaves no trace in this heap (the outgoing document's scripts are
 * queued, its flows parked, its Window answering), and the browser's document id and `sender.tab.url` are the
 * provenance (SECURITY.md). It is HTML §7.4.6.1 "Updating the traversable"'s deactivate a document for a
 * cross-document navigation, not §7.3.1.6 "Navigable destruction": the navigable survives and only its active
 * Document changes. Both reach §7.5.10 "Destroying documents" through §7.5.9 "Unloading documents"' "if
 * oldDocument's salvageable state is false, then destroy oldDocument", so one machine serves both
 * (core/frame/document_lifecycle.h). The order is `qjs_join` then this, as §7.4.6.1 unloads `displayedDocument`
 * given `targetEntry's document`; the zone checks the order over the table it holds (bridge.js).
 * Nothing is freed here: the replaced realm is released by the collector through quickjs's realm-teardown hook
 * once nothing holds it (a parked flow holds it by counted reference, core/frame/navigable.c), and its tree is
 * this host's until `qjs_teardown`. The incoming document's name does not cross: §7.5.9 step 6 queues on the
 * outgoing document's global, and `newDocument` feeds only unload timing info, which this user agent does not
 * carry (step 3 answers its absence). */
QJS_EXPORT void qjs_unload(const char *doc_id)
{
    uint32_t doc;

    DCHECK(g_begun,
           "a Document was reported REPLACED to an engine whose frontier was never seeded — §7.5.9's unload is "
           "a task of every timeline of that document and this instance has none, so the destruction would be "
           "queued onto nothing");
    DCHECK(doc_id != NULL && *doc_id,
           "a Document was reported replaced with no NAME — an instance is an ORIGIN-KEYED AGENT CLUSTER and "
           "holds one realm per same-origin document, so the name is the whole of what says WHICH of them the "
           "browser navigated away from");
    doc = world_doc_intern(doc_id);
    DCHECK(world_doc_hosted(doc),
           "a Document this agent does not hold was reported replaced — the trusted zone routes on which "
           "instance holds which document, so this arrival is at the wrong instance, and unloading anything "
           "here would destroy a document the browser never navigated away from");
    engine_unload_document(doc);
}

/* CSP §4.1.2 "Should request be blocked by Content Security Policy?" for a request to `url` with Fetch §2.2.5
 * destination `destination`, made by document `doc_id`, asked for its answer alone: the Send panel shows the
 * frame's verdict on a request the person composed, and the trusted zone holds no CSP matcher. Nothing is
 * reported and no event is queued (asserted), and no flow runs. The request is a fresh one whose creating
 * algorithm states no CSP metadata (Fetch §5.4 "Request class") and has a redirect count of 0.
 * Answers `allowed`, or `blocked<TAB>` and the §6.8.4 directive that refused it. The string is this entry's
 * and stays valid until its next call.
 * Not covered: a report-only policy. `Content-Security-Policy-Report-Only` is not delivered to this engine
 * (core/frame/csp_directive_list.h), so no policy has disposition "report" and the `report-only` answer is
 * never given. The next diff delivers that header as policies of disposition report and answers
 * `report-only<TAB>directive` when only such a policy is violated. Its absence shows as `allowed` for a request
 * that only a report-only policy refuses. */
QJS_EXPORT const char *qjs_check_request_policy(const char *doc_id, const char *url, const char *destination)
{
    static char *answer;
    const CspDirective *governing;
    CspRequestVerdict verdict;
    unsigned long reports_before;
    UrlRecord rec;
    uint32_t doc;

    DCHECK(g_begun, "a request policy was asked of an engine whose frontier was never seeded");
    DCHECK(doc_id != NULL && *doc_id && url != NULL && destination != NULL,
           "a request policy was asked with no document, no URL or no destination");
    DCHECK(*destination == '\0' || fetch_is_destination_type(destination),
           "a request policy was asked for a destination outside Fetch §2.2.5's enumeration");
    doc = world_doc_intern(doc_id);
    url_record_init(&rec);
    /* The zone sends a URL its own parser serialized, and a serialized URL parses against no base. */
    if (!url_parse(&rec, url, strlen(url), NULL)) {
        DFAIL("a request policy was asked for a URL the URL parser refuses — the zone sends `new URL(x).href`, "
              "which parses against no base");
        url_record_free(&rec);
        return "unknown";
    }
    reports_before = csp_violation_reports();
    verdict = policy_request_verdict(engine_document_policy(doc), &rec, destination,
                                     csp_request_metadata_unstated(), /*redirect count*/ 0, &governing);
    url_record_free(&rec);
    DCHECK(csp_violation_reports() == reports_before,
           "asking CSP §4.1.2 for its answer alone reached §5.5 \"Report a violation\" — the question changed "
           "the document it asked about");
    free(answer);
    if (verdict == CSP_REQUEST_ALLOWED) {
        answer = strdup("allowed");
    } else {
        answer = malloc(sizeof "blocked\t" + governing->name.n);
        CHECK(answer != NULL, "OOM composing a request policy answer");
        memcpy(answer, "blocked\t", sizeof "blocked\t" - 1);
        memcpy(answer + sizeof "blocked\t" - 1, governing->name.p, governing->name.n);
        answer[sizeof "blocked\t" - 1 + governing->name.n] = '\0';
    }
    CHECK(answer != NULL, "OOM composing a request policy answer");
    return answer;
}

/* The frontier key's document half. Pure scan, so the host may ask the instant qjs_init returns. */
QJS_EXPORT unsigned qjs_bundle_id(void)
{
    DCHECK(g_dom != NULL, "qjs_bundle_id was asked before the document was parsed");
    return g_bundle_id;
}

/* The stall question is the engine's, over the engine's registers (engine.h's engine_host_owes); no host restates
   it, so every driver asks both the pending-fetch and host-request registers. */

/* A peer holds a reference into this instance's document, so its timelines may not run out (solver/engine.h's
 * engine_set_referenced). Only the party that provisioned the instance knows another agent created its navigable,
 * so the trusted zone states it; without it a cross-origin child's frontier drains and its creator's first
 * `otherW.length` arrives at a document with no timeline (engine_perform's count assert and engine_route's
 * drained-instance assert name that from the far side). It is stated before the frontier is seeded, since it
 * decides whether the last timeline finishes; wpt_runner.c's child host places it the same way. It is not a
 * session fact: a parked instance resumes into the same provisioning (the routing name is stable across a park,
 * solver/world.h), so it survives qjs_teardown. */
QJS_EXPORT void qjs_set_referenced(int referenced)
{
    DCHECK(g_ctx != NULL, "qjs_set_referenced ran before qjs_init built the context — the statement is about "
                          "THIS instance's document, and there is no document yet for a peer to hold a "
                          "reference into");
    DCHECK(!g_begun, "a peer's reference into this document was declared after the frontier was seeded — the "
                     "flag decides whether the LAST timeline may finish, so a session that has already begun "
                     "may already have taken that decision and closed over the flows this was meant to keep");
    DCHECK(referenced == 0 || referenced == 1,
           "a peer reference was declared with something that is neither of the two answers — the host either "
           "provisioned this instance for a navigable another agent created or it did not, and a third value "
           "would be read by whichever arm the scheduler's test happens to be written as");
    /* A 0 says the provisioning gave no reference. It leaves standing one the parse inside qjs_init already
       raised: markup that creates a cross-origin `<iframe>` makes this document the child's `parent`. */
    if (referenced) engine_set_referenced(1);
}

/* Whether a peer holds a reference into this instance's document, read off solver/engine.c's one flag. It is
   raised by qjs_set_referenced and by the engine itself when this document creates a navigable another agent
   hosts (core/frame/navigable.c). A native host's stall arm asks this. */
QJS_EXPORT int qjs_referenced(void)
{
    DCHECK(g_ctx != NULL, "qjs_referenced was asked before qjs_init built the context");
    return engine_referenced();
}

/* Phase 2: seed the frontier. */
QJS_EXPORT void qjs_begin(const char *recipes)
{
    DCHECK(g_ctx != NULL, "qjs_begin ran before qjs_init built the context");
    DCHECK(!g_begun, "qjs_begin ran twice — the frontier is seeded once and stepped thereafter");
    /* The parked residue seeds the frontier instead of the boot flow (solver/cold.h). Each stored recipe names the
       path one suspended flow had taken, as arms over shared frozen segments, and resumes by replaying that path
       while re-running this document from its first script; the delta, DOM writes, frames, constraint, chunks
       and owed replies are regenerated by the replay, while example values track today's server. */
    engine_sched_begin(g_ctx, g_scripts.bodies, g_scripts.srcs, g_scripts.types, g_scripts.sched, g_scripts.els,
                       g_scripts.n, /*forking*/1, recipes);
    g_begun = 1;
}

/* One cooperative quantum of the one dispatch loop, answering the scheduler's three codes, which are three
 * different things to do next. ENGINE_STEP_YIELD leaves every flow where it was and the frontier runnable: the
 * thread is asked for, so a host with nothing else to do steps straight back in. ENGINE_STEP_DONE means the frontier
 * is empty and the session's hooks are uninstalled. ENGINE_STEP_STALLED means every member is parked on something
 * only this host can supply: a yield asks to be outranked and a stall asks to be paid, and stepping again before
 * paying converts nothing into work (a driver that treats a stall as a yield spins). */
QJS_EXPORT int qjs_step(void)
{
    int r;

    DCHECK(g_begun, "qjs_step ran before the frontier was seeded");
    if (g_done)
        return ENGINE_STEP_DONE;   /* the session is over; stepping it again is not a question to ask twice */
    r = engine_sched_step();
    /* The other half of the slice invariant: preempt_hook asserts the engine executes only inside a slice, and
       this asserts the slice is closed before the thread returns to the host, so every entry below (provide,
       route, perform, answer, result, teardown) is host time by construction. engine_sched_step brackets exactly
       one slice around one call, so a hit means something opened a slice outside it. */
    DCHECK(!quantum_slice_open(),
           "qjs_step is returning to the host with the cooperative quantum's slice still OPEN — the host now "
           "pumps its port, streams findings and delivers replies on time the scheduler believes a flow is "
           "holding, and the CPU edge is still armed over it");
    /* The flow stamp is down. Every entry below runs on the host's time and creates objects (qjs_provide parses a
       reply and dups it onto parked flows' pending registers; qjs_host_answer parses an answer; qjs_route and
       qjs_perform build a delivery). With the stamp up they would carry the yielded flow's generation, and
       `JS_ObjFlowGen(obj) > d->fork_gen` would skip them in every delta, so a later write by a sharing flow would
       be captured nowhere. Nothing inside the engine can see that, so it is asserted at the boundary. */
    DCHECK(JS_FlowGen() == 0,
           "qjs_step is returning to the host with the flow stamp still up — every object the host now creates "
           "would be stamped as belonging to the flow that just yielded, and a write to one of them by any "
           "OTHER flow that shares it is skipped by that flow's delta and survives its rewind");
    /* The capture route is down too, the other half of the same fact: a baseline object is the shared arm of the
       capture test, so with the route still on the yielded flow, every property an entry below writes on a record
       it built is recorded as a creation in that flow's head, and its next context switch deletes them (a reply
       would reach its flow as `{}`). The two asserts are opposite failures of one boundary. */
    DCHECK(cow_current() == NULL,
           "qjs_step is returning to the host with the last flow's CAPTURE ROUTE still up — every field the "
           "host writes on a record it builds is recorded as a creation in that flow's COW head, and its next "
           "context switch deletes them, so the reply arrives at its delivery stripped to an empty object");
    /* Three values, and this entry is where that holds: the membership check stops a fourth from reaching a host
       whose branches are written against these three. */
    DCHECK(r == ENGINE_STEP_DONE || r == ENGINE_STEP_YIELD || r == ENGINE_STEP_STALLED,
           "the scheduler answered a step with a code this ABI does not carry — the host branches on DONE, "
           "YIELD and STALLED, so a fourth value reaches it as whichever branch happens to be the fallback");
    /* A stall is a bill, so it names what is owed: the two registers this ABI's host can read. There are two stall
       causes. engine_sched_step stalls for engine_host_owes (both registers' union; its dev walk asserts every
       outstanding entry is tellable through one) and for a referenced document, whose last timeline waits for a
       question arriving through qjs_perform or qjs_route and owes no register entry. The third disjunct is the
       engine's reference flag, raised by qjs_set_referenced or by the engine when this document created a
       navigable another agent hosts. An unreferenced instance stalling with both registers empty is the failure
       named. */
    DCHECK(r != ENGINE_STEP_STALLED || engine_referenced() ||
           *engine_pending_fetches() != '\0' || *engine_host_requests() != '\0',
           "the scheduler asked this host to PAY and named nothing owed — a stall reaches the host as a bill "
           "over qjs_pending and qjs_host_requests, and with both empty there is no record to answer, so every "
           "flow parked here waits for the rest of the session on a payment nothing identifies. This instance "
           "declared no peer reference, so it is not the referenced case either: nothing is waiting to be "
           "asked a question here, and nothing is owed a reply");
    g_done = (r == ENGINE_STEP_DONE);
    return r;
}

/* The one result document, serialized directly from the C findings; the host does one JSON.parse of it. This entry
   owns the composition: the previous answer is released before a new one is asked for, and the last at teardown. */
QJS_EXPORT const char *qjs_result(void)
{
    DCHECK(g_begun, "qjs_result was asked of an engine that never ran");
    free(g_result);   /* the previous answer: the host has read it or not, and either way it asked again */
    g_result = result_json(g_ctx);
    /* At the origin, where the composition failed: a null crosses this ABI as the empty string (the binding
       converts address zero to ""), which would read as an engine that ran and found nothing. result_json answers
       nothing only when the composition could not be allocated, which is always fatal. */
    CHECK(g_result != NULL, "the result document could not be composed — every endpoint and every verified sink "
                            "this page produced is discarded with it");
    return g_result;
}

/* Whose world the heap is standing in now, composed at the instant the render is taken. `flow_running()` names the
 * timeline the next `document_paint` will see: solver/engine.c's three writers of that stamp pair it with the delta
 * operation (`flow_switch_in` after `cow_apply`, `flow_switch_out` after `cow_unapply`, `flow_finish`), the DOM
 * twin travels in the same pair, and core/paint/document_paint.c reads only the Lexbor tree and the viewport.
 * It is the one mark `qjs_step` leaves up across this ABI (core/loader/document_load.c: a yielded flow's delta is
 * still applied), so it cannot be cross-checked against `cow_current()`, which is taken down between slices.
 * NULL is the baseline, which is a world: a drained session switches its last flow out, so the run stands on the
 * document as no flow wrote it. It is stated as the word `baseline`, which has no colon, so `world_parse` refuses
 * it by name (solver/world.h's grammar is `doc:session:serial`). It does not say why (drained or not yet run);
 * the host holds the step code. Allocated on both arms; freed at `qjs_paint` and at teardown. The stamp is read
 * once in `qjs_paint` and handed in, because two registers are written off it. */
static char *paint_world_name(Flow *f)
{
    char *s;

    if (f == NULL) {
        s = strdup("baseline");
        CHECK(s != NULL, "the world an image was rendered in could not be named — a picture that cannot say "
                         "which timeline it is of is one nothing downstream can attribute, and under this "
                         "engine one document has as many appearances as it has flows");
        return s;
    }
    return world_name(f->world);
}

/* What that world's appearance is worth, a second question asked of the same stamp: solver/flow.h's
 * `path_forced`, whether the path took an arm its own concrete example contradicts, in which case the picture is
 * of a page no session reaches. It does not route to `engine_prov_of_running_path`, which answers PROV_DERIVED for
 * a flow-less act because a provenance may only under-claim (solver/engine.h); a baseline image is a document in
 * which no page code has run, so for a picture the flow-less arm answers `baseline`. The answer is a word, so a
 * caller cannot read it as "is this the browser's picture", and a fourth state fits without a type change.
 * Named residual. Not covered: `unforced` does not separate a path whose examples agreed from one that branched over
 * an example-free value (external input, absent server state), where decide_real_arm answers REAL_ARM_UNOBSERVED
 * and both siblings answer `unforced`. Next diff builds: an arm separating "the example contradicts this" from
 * "there is no example", recorded beside `path_forced` and answered here as a fourth word; `flow_path_pinned`
 * covers only an unobserved arm that pinned a value. How its absence would show: two images of one document
 * both graded `unforced`, so a reference comparison cannot tell "this engine is wrong" from "this question has
 * no answer here".
 * No allocation: every arm answers one of this file's constants, so the `#` comment line needs no newline check. */
static const char *paint_forced_word(Flow *f)
{
    if (f == NULL) return "baseline";
    return flow_path_forced(f) ? "forced" : "unforced";
}

/* An image of this instance's document: CSS 2.1 §E.2 "Painting order"'s ink composited onto a surface the size CSS
   2.1 §2.3.1 "The canvas" establishes, returned as the address of core/graphics/raster_surface.h's
   non-premultiplied RGBA8 run (four bytes per pixel, R G B A, row-major), assembled by core/paint/document_paint.h.
   It performs the render: asking twice renders twice. It is an output register, not per flow: it renders whichever
   world is current, and cannot be asked for a world by name. `qjs_request_paint_every_world` (below) marks
   timelines, and the scheduler yields the thread with each marked member standing, so a host renders each by
   calling this; the scheduler performs no switch for a picture (`flow_switch_in` records what a member was ranked
   on). The register holds one image: the host that asked renders and writes it before the next, and an image per
   member would be the per-flow materialization the rules forbid (`4 * qjs_paint_width() * qjs_paint_height()`
   bytes against thousands of members). At the end of a run the stamp is NULL and both words read `baseline`.
   The pair is ordered: `qjs_paint_bytes` aborts if asked before a render. The address is valid until the next
   `qjs_paint` or `qjs_teardown`, and the host never frees it. NULL is an answer, a document with no image
   (raster_surface_bytes treats an absent run and a zero extent as one fact). Whether that is no rendered region or
   one of no size is a fact about the viewport, which core/frame/viewport.h answers from this realm; no
   provisioning party states a viewport (`qjs_init` takes none), so `count` is asserted rather than discarded. */
QJS_EXPORT const uint8_t *qjs_paint(void)
{
    bool region;
    Flow *f;

    DCHECK(g_ctx != NULL && g_dom != NULL,
           "an image was asked of an instance that was never initialised — there is no document to render and "
           "no realm holding the viewport whose region would size it");
    /* The previous image, released before a new one, as in `qjs_result`. A zero-area surface frees nothing, so this
       is correct on the first call too. */
    raster_surface_free(&g_paint);
    /* The previous image's world with it: the two are one artifact. */
    free(g_paint_world);
    /* The running-flow stamp, read once, because two facts about this render are written off it and must be about
       one world (`paint_world_name` says why this register). The membership claim is stated with the read: a flow
       that has left the frontier had its world's death announced (solver/world.h's world_flow_gone), and
       flow_release runs after flow_finish's `flow_set_running(NULL)`, so this fires only if one of the stamp's
       three writers came apart. */
    f = flow_running();
    DCHECK(f == NULL || flow_is_member(f),
           "the running-flow stamp names a flow the frontier no longer holds — the image about to be rendered "
           "would be attributed to a timeline whose death has already been announced to every peer");
    g_paint_world = paint_world_name(f);
    /* What that timeline's appearance is worth, on the adjacent line from the same read, since the two are one
       artifact. */
    g_paint_forced = paint_forced_word(f);
    region = document_paint(g_ctx, g_dom, &g_paint, &g_paint_count);
    g_painted = 1;
    DCHECKF(region || (g_paint_count.census.offers == 0u && g_paint_count.marks == 0u &&
                       !g_paint_count.complete),
            "core/paint/document_paint.h answered that CSS 2.1 §2.3.1 \"The canvas\" establishes no rendered "
            "region for this document and then reported a walk over it — %u offer(s), %zu mark(s), complete=%d. "
            "The false arm is stated to return before the walk, so a count with anything in it is that entry "
            "having painted a document it had already said has nowhere to be painted",
            g_paint_count.census.offers, g_paint_count.marks, g_paint_count.complete ? 1 : 0);
    return g_paint.px;
}

/* The length of the run `qjs_paint` last answered. As core/graphics/raster_surface.h says, "Non-premultiplied
   RGBA8 contains 0x00 by construction — every fully transparent pixel is four of them, and every black channel is
   one — so a byte run of pixels has no spelling as a C string and its extent can be carried only as a number beside
   it." It asserts that a paint happened, because zero is already the answer for a document with no image. */
QJS_EXPORT unsigned qjs_paint_bytes(void)
{
    size_t n;

    DCHECK(g_painted,
           "the extent of an image was asked before one was rendered. The pair is ordered — `qjs_paint` "
           "produces and this states how much it produced — and a host that asks this first is reading a "
           "register no render has written, whose zero is indistinguishable from a document with no picture");
    n = raster_surface_bytes(&g_paint);
    /* The one narrowing, a CHECK because truncation would make the host copy a prefix or read past the buffer.
       Every length crosses this ABI as `unsigned` (`html_len`, `body_len`). It cannot fail where `size_t` is 32
       bits (the wasm host, where `raster_surface_init`'s overflow refusal binds); it is for native drivers. */
    CHECKF(n <= (size_t)UINT_MAX,
           "a rendered image is %zu bytes, which this ABI states lengths too narrow to carry — a host would "
           "copy a prefix of the picture and believe it had the whole of it", n);
    return (unsigned)n;
}

/* The shape of that run, which the extent does not determine: `4 * width * height` has many factorisations (3686400
   bytes is 1280 x 720, 720 x 1280 or 960 x 960), and every image container states its dimensions in its header
   (`engine/host/test_forced.c`'s `--paint-dir` writes one). Two entries, not one packed answer, so a transposed
   read is a different call, as qjs_abi.h argues for the pointer and the count. They are the surface's own, the
   one core/paint/document_paint.h initialised through core/paint/display_list_raster.h's device-pixel rounding,
   not re-derived from a viewport. Zero is an answer: a document CSS 2.1 §2.3.1 "The canvas" establishes no region
   for has a zero-area surface, which `qjs_paint_bytes` and a NULL from `qjs_paint` describe too. */
QJS_EXPORT unsigned qjs_paint_width(void)
{
    DCHECK(g_painted,
           "the width of an image was asked before one was rendered — `qjs_paint` produces and this states "
           "part of the shape of what it produced, so a host that asks this first is reading a register no "
           "render has written, whose zero is indistinguishable from a document with no picture");
    DCHECKF(g_paint.width >= 0,
            "the rendered surface reports a width of %d. core/graphics/raster_surface.h's extent is "
            "two-sided against its own allocation, so a negative dimension is that allocation having been "
            "made from a number no region produced", g_paint.width);
    return (unsigned)g_paint.width;
}

QJS_EXPORT unsigned qjs_paint_height(void)
{
    DCHECK(g_painted,
           "the height of an image was asked before one was rendered — the pair is ordered exactly as "
           "`qjs_paint_bytes` states, and a register no render has written answers zero for a picture that "
           "was never attempted");
    DCHECKF(g_paint.height >= 0,
            "the rendered surface reports a height of %d — see the width's own assert for why a negative "
            "dimension is this engine's allocation and never a region any medium established", g_paint.height);
    return (unsigned)g_paint.height;
}

/* Why the ink is there, which a bitmap cannot be asked: core/paint/document_paint.h's `DocumentPaintCount` made
   readable (no ink means the walk reached no box, reached boxes that painted nothing, or was stopped).
     · `offers` is core/paint/box_paint.h's count of steps CSS 2.1 §E.2 "Painting order"'s walk offered; zero is a
       walk that never happened, since paint_order.c offers step 1 for every root it walks.
     · `marks` is what was laid: zero offers with zero marks is an empty document; offers with zero marks is a
       walk every box of which painted nothing.
     · `complete` false is a painter that met an operand it could not compute and stopped, leaving a partial
       picture that a host must not publish as the page.
   core/paint/display_list_raster.h's `spans` and `pixels` are rasterizer work counters, not exposed: `pixels`
   counts overlap once per mark, so it would be misread as an area. The fixture reads both directly. */
QJS_EXPORT unsigned qjs_paint_offers(void)
{
    DCHECK(g_painted,
           "what CSS 2.1 §E.2 \"Painting order\"'s walk offered was asked before any walk ran — the register "
           "is written by `qjs_paint` and its zero is that section's answer for a root that generates no box, "
           "so an unwritten one reports a document as empty that was never looked at");
    return g_paint_count.census.offers;
}

QJS_EXPORT unsigned qjs_paint_marks(void)
{
    DCHECK(g_painted,
           "how much ink the last render laid was asked before one was rendered — zero is a real answer for a "
           "document every box of which painted nothing, so a register no render has written reports that "
           "finding for a page nobody painted");
    /* A DCHECK, unlike the extent's CHECK: `UINT_MAX` marks would be tens of gigabytes of `DisplayMark`s, whose
       allocation CHECK fails first, so this guards a count gone wrong; the extent's failure is a host reading past
       a buffer. */
    DCHECKF(g_paint_count.marks <= (size_t)UINT_MAX,
            "the last render laid %zu marks, which this ABI states counts too narrow to carry — the list "
            "holding that many would have failed its own allocation first, so this is a count that lost its "
            "derivation rather than a document that drew this much", g_paint_count.marks);
    return (unsigned)g_paint_count.marks;
}

QJS_EXPORT int qjs_paint_complete(void)
{
    DCHECK(g_painted,
           "whether the last render finished was asked before one was rendered — FALSE is the answer for a "
           "painter that STOPPED, so an unwritten register reports a partial picture for a document that has "
           "no picture at all");
    return g_paint_count.complete ? 1 : 0;
}

/* The three above broken apart: core/paint/box_paint.h's two populations and the document's size. The partition
   (outcomes, which sum to the offers) and the tally (reasons, which sum to nothing) are two entries, so reading a
   reason as a share of the offers is a call mistake rather than an index mistake (qjs_abi.h). The index is
   asserted, not clamped: both enums are closed lists this engine writes and every host includes, and a clamp would
   answer another bucket under the caller's name. The release arm reads an in-range bucket. */
QJS_EXPORT unsigned qjs_paint_offer_outcome(unsigned outcome)
{
    DCHECK(g_painted,
           "what became of the last render's offers was asked before one was rendered — every bucket of an "
           "unwritten census answers zero, and a partition that is zero everywhere reads as a walk that made "
           "no offers rather than as a walk nobody asked for");
    DCHECKF(outcome < (unsigned)BOX_PAINT_OUTCOMES,
            "outcome %u was asked of CSS 2.1 §E.2 \"Painting order\"'s census, which holds %u. "
            "core/paint/box_paint.h's `BoxPaintOutcome` is a CLOSED list this engine writes and every host of "
            "this ABI includes that header, so an index outside it is this engine's own memory rather than a "
            "host asking about a bucket that might exist", outcome, (unsigned)BOX_PAINT_OUTCOMES);
    return g_paint_count.census.outcome[outcome < (unsigned)BOX_PAINT_OUTCOMES ? outcome : 0u];
}

QJS_EXPORT unsigned qjs_paint_decline(unsigned reason)
{
    DCHECK(g_painted,
           "why the last render's offers laid no ink was asked before one was rendered — zero is a real "
           "answer for every reason at once (a document each of whose boxes painted), so an unwritten census "
           "reports that finding for a page nobody painted");
    DCHECKF(reason < (unsigned)BOX_PAINT_DECLINES,
            "reason %u was asked of CSS 2.1 §E.2 \"Painting order\"'s census, which holds %u — see the "
            "outcome entry above for why an index outside a closed enum this engine writes is not a question "
            "a host can legitimately ask", reason, (unsigned)BOX_PAINT_DECLINES);
    return g_paint_count.census.decline[reason < (unsigned)BOX_PAINT_DECLINES ? reason : 0u];
}

QJS_EXPORT unsigned qjs_paint_elements(void)
{
    DCHECK(g_painted,
           "the size of the tree the last render walked was asked before one was rendered — this is the "
           "DENOMINATOR the offer count is read against, so an unwritten register answers zero and makes "
           "every walk look like a walk over an empty document");
    /* At least one whenever the walk ran: the count is over the root's shadow-including inclusive descendants, so
       zero elements beside non-zero offers is two readings taken of two trees. Zero with zero offers is the arm
       where CSS 2.1 §2.3.1 establishes no region. */
    DCHECKF(g_paint_count.elements >= 1u || g_paint_count.census.offers == 0u,
            "CSS 2.1 §E.2 \"Painting order\"'s walk made %u offer(s) over a tree this host counts %u "
            "element(s) in. Both are readings of the SAME subtree at the SAME instant — the walk's own and "
            "core/paint/document_paint.c's — so offers over no elements is the pair having been taken of two "
            "different documents", g_paint_count.census.offers, g_paint_count.elements);
    return g_paint_count.elements;
}

/* Which world that picture is of: solver/world.h's `doc:session:serial`, or `baseline` for the document as no flow
   wrote it (see `paint_world_name`). It asserts a render happened, as its neighbours do, and because a NULL would
   read like the baseline arm to a consumer testing for absence. It answers for the render `qjs_paint` last
   performed, never for this instance now, since a step may switch in another flow. */
QJS_EXPORT const char *qjs_paint_world(void)
{
    DCHECK(g_painted,
           "the world an image was rendered in was asked before one was rendered — the pair is ordered exactly "
           "as `qjs_paint_bytes` states, and an unwritten register answers NULL, which a consumer reads as the "
           "field being absent rather than as this engine never having been asked to paint");
    DCHECK(g_paint_world != NULL,
           "a render has happened and left no world beside it — `qjs_paint` writes both registers on adjacent "
           "lines, so a picture with no timeline on it is that pair having come apart and an image nothing "
           "downstream can attribute");
    return g_paint_world;
}

/* Whether the path that laid that picture's ink took an arm its own concrete example contradicts: `forced`,
   `unforced`, or `baseline` (see `paint_forced_word`, including its named residual). It asserts a render happened
   for `qjs_paint_world`'s reasons, and comes from the same read of the running-flow stamp that named the world. The
   pointer is a constant of this file, and the host never frees it. */
QJS_EXPORT const char *qjs_paint_forced(void)
{
    DCHECK(g_painted,
           "what the path that laid an image's ink stands on was asked before an image was rendered — the "
           "pair is ordered exactly as `qjs_paint_bytes` states, and an unwritten register answers NULL, "
           "which a consumer reads as the field being absent rather than as this engine never having been "
           "asked to paint");
    DCHECK(g_paint_forced != NULL,
           "a render has happened and left no statement of what its path stands on — `qjs_paint` writes this "
           "register and the world's name beside it on adjacent lines and off ONE read of the running-flow "
           "stamp, so a picture carrying a timeline and no grade is that pair having come apart");
    return g_paint_forced;
}

QJS_EXPORT void qjs_teardown(void)
{
    DCHECK(g_dom != NULL, "qjs_teardown ran on an instance that was never initialised");
    /* A flow waiting on a reply holds a snapshot, a COW delta and a parked continuation. flow_finish asserts a flow
       may not finish with a continuation parked, and teardown would evade that, so the rule is stated here:
       the host owes what qjs_pending told it, unless the frontier was paged out, since a parked recipe replays the
       code that issued the request and re-makes it in the resuming session. A DONE that closes over live members
       writes them down first (engine_sched_slice's close), so stepping to DONE is one of the two exits that park.
       Named residual: the predicate asks the host question and means the flow one. Not covered: a teardown by a
       host that neither parked nor stepped to DONE, when every live member is parked on a refusal, since
       `engine_pending_fetches` skips refused entries and the assert passes. Next diff builds: the pair asked over
       the frontier (a flow still holding a snapshot), which solver/engine.h can answer. How its absence would
       show: a `_park` document naming fewer flows than the census reported live at the last step, with both
       asserts silent. */
    DCHECK(engine_frontier_paged() || *engine_pending_fetches() == '\0',
           "qjs_teardown with replies still owed — every flow parked on one is dropped with its continuation. "
           "Provide them, step to DONE, or park the frontier, before ending the session");
    DCHECK(engine_frontier_paged() || *engine_host_requests() == '\0',
           "qjs_teardown with SYNCHRONOUS requests still owed — a flow blocked on one is suspended mid-frame "
           "with its snapshot intact, and tearing down drops it exactly where the pending assert above says "
           "not to. Answer them, step to DONE, or park the frontier, before ending the session");
    /* This is where the ABI host stops stepping, so the session ends here (solver/engine.h's engine_sched_end):
       an exit other than DONE, such as a referenced instance resting on a stall, leaves one flow switched in,
       and the release below would free its live state as if it were parked. Before the scripts, which the
       session borrows. After DONE it does nothing. */
    engine_sched_end();
    doc_scripts_free(&g_scripts);
    /* The order and membership of this list match test_forced.c's main(), whose gate walks gc_obj_list. */
    /* The frontier first: a suspended flow's heap-frame chain holds the step machines of browser algorithms it is
       stopped inside, and tearing one down runs that component's `fini`, which reads agent state the platform's
       release gives back (solver/engine.h). */
    solver_frontier_free(g_ctx);
    /* The platform's own release column, in reverse declaration order (core/platform.h), which covers every
       component's agent-lifetime state; see core/platform.c for the order of each group. */
    platform_agent_free();
    document_free(g_ctx);   /* the Document and the window it fires `load` at, both held across the lifecycle */
    /* This host's reference to every joined realm, given back here. A joined document's per-realm record is
       released by quickjs's realm-teardown hook, which core/frame/navigable.c installs for every realm of this
       agent, when the realm's graph is collected (the JS_RunGC below, or JS_FreeRuntime); releasing the record
       first would leave a live realm whose Document is gone. `g_ctx` is the realm this host stands in and frees
       by hand at the end, so its record is released by the line above. */
    {
        int i;
        for (i = 0; i < g_joined_n; i++) JS_FreeContext(g_joined_ctx[i]);
    }
    /* `document_free` above releases a realm's record rather than the agent's (it reads `doc_of(ctx)` and clears
       this context's opaque), which the platform column cannot express; `document_agent_free` on the column gives
       back §4.5's class, declarations, ten sub-components and §13.2.7 lifecycle claim. The two are not ordered
       against each other, since a child navigable's document_free runs from quickjs's realm-teardown hook; the
       per-realm half reads no static of core/dom/document.c, which asserts it at its release. */
    realm_intrinsics_free();   /* the declarations are the agent's; each realm's prototypes went with it */
    navigable_free(g_ctx);
    /* The solver's own release list, one call in solver/engine.h, as the platform's is. */
    solver_agent_free(g_ctx);
    /* After the frontier, because a flow parked inside an IDL member reads this pool at its teardown;
       idl_args_free asserts the ordering. */
    idl_args_free(g_ctx);   /* the dictionary member atoms the declaration pool interned */
    /* The world registry is released by flow_registry_free inside solver_agent_free, so it is not named here. */
    /* The collection is part of the teardown: a run leaves unreachable flow-local garbage, and gc_obj_list counts
       it as it counts a real leak. */
    if (g_rt) JS_RunGC(g_rt);
    if (g_ctx) { JS_FreeContext(g_ctx); g_ctx = NULL; }
    if (g_rt)  { JS_FreeRuntime(g_rt);  g_rt  = NULL; }
    /* After JS_FreeRuntime, because the order is part of the meaning: JS_RegisterStepDef borrows a step definition
       that must outlive the runtime (its [stepleak] report reads `def->steps` to name each unfinished machine), and
       each IDL pool block holds a definition the runtime borrowed. */
    idl_args_pool_free();
    idl_async_iter_free();
    /* Every document of this agent, the joined ones first, for the arenas' sake: core/dom/node_heap.h puts node
       storage on the agent's heap, destroyed with the last document that gives up its claim, so a joined tree left
       undestroyed would hold the agent's whole node heap. */
    {
        int i;
        for (i = 0; i < g_joined_n; i++) dom_document_destroy(g_joined_dom[i]);
    }
    free(g_joined_ctx); g_joined_ctx = NULL;
    free(g_joined_dom); g_joined_dom = NULL;
    g_joined_n = g_joined_cap = 0;
    dom_document_destroy(g_dom);
    g_dom = NULL;
    /* The last answer, this file's allocation, freed after the arrays above because it is the one host allocation
       that outlives the context it was composed from; the host reads qjs_result before teardown. */
    free(g_result);
    g_result = NULL;
    /* The last image, this file's for the same reason: outside the runtime's heap, so only this line releases it,
       after the realm because the surface outlives the objects the paint read. `g_painted` goes with it, so a
       resumed session that painted nothing does not answer a length from the previous one's register. */
    raster_surface_free(&g_paint);
    /* The name of the world it was taken in, released with the pixels it describes. */
    free(g_paint_world);
    g_paint_world = NULL;
    /* The grade of that image's path is a constant, so it is cleared rather than freed; the entry reading it
       asserts non-NULL, so a surviving register would answer about the previous session's render. */
    g_paint_forced = NULL;
    g_painted = 0;
    g_begun = 0;
    g_done = 0;
}

/* ---- The capabilities this entry cannot reach yet -----------------------------------------------------
 * Each aborts at its own entry naming what to build. Answering "" or 0 would have the host believe an engine that
 * reports no pending fetches while a flow is parked on one that never arrives. */

/* What the trusted zone still owes the frontier, as `METHOD<TAB>DESTINATION<TAB>INITIATOR<TAB>PROVENANCE<TAB>
   PINNED<TAB>DRIVEN<TAB>CREDENTIALS<TAB>MODE<TAB>REFERRER_POLICY<TAB>REFERRER<TAB>URL` lines; engine_pending_split is the
   authority on the grammar. The method, the mode and the referrer pair are part of the request's identity
   (solver/engine.h), so a GET and a POST to one address are two requests; the zone issues each line as it names it
   and hands (method, mode, referrer policy, referrer, url) back to qjs_provide. The destination is Fetch §2.2.5
   Requests' and travels on this line, so the chokepoint classifies a document's own `<script src>` as well as a
   dynamic `import()`. */
QJS_EXPORT const char *qjs_pending(void)
{
    DCHECK(g_begun, "qjs_pending was asked of an engine that never ran");
    return engine_pending_fetches();
}

/* The reply's metadata crosses as JSON text, as qjs_host_answer's does: `{status, statusText, headers: [[name,
   value], …], urlList: [url, …]}`, the same record fetch_reply_new builds for hosts that fetch in C, so
   `response.url` and `response.redirected` reflect the fetch's redirects. JSON `null` is a network error, which the
   delivery machine rejects with the TypeError §5.6 names. The body crosses beside it as bytes, because §2.2.4
   Bodies makes a body's source a byte sequence and JSON cannot carry one without running an algorithm over it: a
   script served `charset=windows-1252` must reach HTML §8.1.4.2's classic decode undecoded. The trusted zone owns
   SOP/CORS/PNA/CORB and no decodes; the bytes are copied into this instance's linear memory and read without a
   transform. A network error carries none (`body == NULL`, `body_len == 0`), distinct from a 204's empty body.
   One delivery for every parked request: a fetch's reply, the document's external script and a lazy chunk's source
   all settle a park registered before suspending, and one engine_provide fills every entry naming that request. */
QJS_EXPORT void qjs_provide(const char *method, const char *mode, const char *referrer_policy,
                            const char *referrer, const char *url, const char *reply,
                            const char *body, unsigned body_len)
{
    DCHECK(g_begun, "a reply was provided to an engine that never ran");
    /* The request this answers is its five-field identity, in the order the join emitted it; a zone calling with
       fewer fields lands its URL in another field, which engine_provide's mappings refuse. */
    DCHECK(method != NULL && mode != NULL && referrer_policy != NULL && referrer != NULL,
           "a reply was provided with no METHOD, MODE, REFERRER POLICY or REFERRER — qjs_pending answers "
           "`METHOD<TAB>DESTINATION<TAB>INITIATOR<TAB>PROVENANCE<TAB>PINNED<TAB>DRIVEN<TAB>CREDENTIALS<TAB>MODE<TAB>"
           "REFERRER_POLICY<TAB>REFERRER<TAB>URL` and this entry takes (method, mode, referrer policy, referrer, "
           "url); a host sending less is answering a request it cannot name");
    DCHECK(reply != NULL, "a reply was provided with no text at all — a network error is the JSON `null`, "
                          "which is a value the engine's delivery distinguishes from a reply it never got");
    DCHECK(body != NULL || body_len == 0,
           "a reply's body arrived as a null pointer with a length — the pointer and the length describe ONE "
           "byte sequence, so a host with an empty body passes an address and a zero, and a host with no reply "
           "at all passes the JSON `null` beside a null pointer");
    {
        JSValue v = JS_ParseJSON(g_ctx, reply, strlen(reply), "<reply>");
        int n;
        if (JS_IsException(v)) {
            JS_FreeValue(g_ctx, JS_GetException(g_ctx));
            /* Aborts in dev: a host sending anything but its reply record is the bug. In release there is no reply,
               and no reply is a network error, which the delivery machine rejects with §5.6's TypeError. */
            DFAIL("the host provided a reply that is not JSON — the trusted zone stringifies its reply record, "
                  "and a bare body sent through this edge is a host still delivering only bytes");
            v = JS_NULL;
        }
        /* §2.2.4 Bodies' body onto §2.2.6 Responses' response, in core/fetch/fetch.h, which owns both. A network
           error has no response, so a body beside one is a host answering a failure and a payload at once. */
        if (JS_IsObject(v))
            fetch_reply_set_body(g_ctx, v, (const uint8_t *)body, (size_t)body_len);
        else
            DCHECK(body_len == 0,
                   "a NETWORK ERROR arrived carrying bytes — Fetch §2.2.6 Responses defines a network error as "
                   "a response whose body is null, and the delivery machine rejects on it rather than reading "
                   "one, so these bytes name a reply the trusted zone did and did not have at the same time "
                   "(§5.6 stood on this sentence and is Fetch methods, where fetch() REJECTS over a network "
                   "error rather than where one is defined)");
        n = engine_provide(g_ctx, method, mode, referrer_policy, referrer, url, v);
        JS_FreeValue(g_ctx, v);
        /* Nobody is parked on it, which happens two ways. The assert is for a host naming a request no flow asked
           for (its pending/provide pairing off, leaving the parked flow waiting forever). The other is a sale: at
           the RAM floor the engine paged the waiting flow out (engine_take_paged_owed), since its recipe re-issues
           the request next session; one sale is consumed per reply it made unnecessary. The take consumes a sale,
           so the condition runs in every build and only the message is dev-only. */
        if (n == 0 && !engine_take_paged_owed()) {
            /* Naming both halves: a zone that fetched the right address with the wrong verb produces this, and a
               message naming the URL alone would send the reader to a URL that is on the list. */
            DFAILF("a reply was provided for a request no flow is parked on and none was paged out — the "
                   "host's pending/provide pairing is off, and resolving nothing leaves the flow that IS "
                   "parked waiting forever. request=%s %s %s", method, mode, url);
        }
    }
}

/* The zone refusing to ask, which is an answer, not a reply (solver/engine.h's engine_decline; safe-fetch.js
   grades the axis). The pairing assert is engine_decline's; qjs_provide keeps its own only because it holds the
   reply value across the paged-sale credit. */
QJS_EXPORT void qjs_decline(const char *method, const char *mode, const char *referrer_policy,
                            const char *referrer, const char *url, const char *reason)
{
    DCHECK(g_begun, "a request was refused to an engine that never ran");
    DCHECK(method != NULL && mode != NULL && referrer_policy != NULL && referrer != NULL && url != NULL,
           "a refusal arrived naming no request — qjs_pending answers `METHOD<TAB>DESTINATION<TAB>INITIATOR<TAB>"
           "PROVENANCE<TAB>PINNED<TAB>DRIVEN<TAB>CREDENTIALS<TAB>MODE<TAB>REFERRER_POLICY<TAB>REFERRER<TAB>URL` and a refusal "
           "answers the same five-field identity a reply does, because it answers the same question");
    DCHECK(reason != NULL && *reason != '\0',
           "a refusal arrived with no REASON — the flow it refuses will not drain this session, so the reason "
           "is the only account a reader ever gets of it, and it is what says whether a per-origin widening "
           "would make this request fire");
    engine_decline(g_ctx, method, mode, referrer_policy, referrer, url, reason);
}

/* What only the trusted zone can answer, as `id<TAB>op` lines: a cross-document operation (creating a navigable,
   reading through a WindowProxy) is answered by the instance holding that document, which only the offscreen
   knows. The asking flow is suspended until qjs_host_answer lands, so this is pulled every step beside
   qjs_pending, and an unanswered request parks that flow while its siblings run. */
QJS_EXPORT const char *qjs_host_requests(void)
{
    DCHECK(g_begun, "qjs_host_requests was asked of an engine that never ran");
    return engine_host_requests();
}

/* What the engine tells the trusted zone, one-way, as `op` lines, drained by the read. A document created here
   (§4.8.5 "The iframe element"'s child navigable, §7.2.2.1 "Opening and closing windows"'s popup) is announced
   rather than negotiated: its name is minted in this instance, and the host provisions an instance under it and
   routes to it. Pulled every step beside qjs_host_requests; a dropped notice is a document nothing runs, and every
   read through it parks its flow. */
QJS_EXPORT const char *qjs_host_notices(void)
{
    DCHECK(g_begun, "qjs_host_notices was asked of an engine that never ran");
    return engine_host_notices();
}

/* The answer crosses as JSON text so it carries its type (`null` is not "null", `0` is not "0") and can have
   structure: §7.4 step 14's load is answered with `{body, csp}`, since a policy is a property of the response, and
   a cross-origin `otherW.length` is a number, as SECURITY.md states for the cross-instance encoder. An answer is a
   completion, not a value: `completion` is 0 for normal and 1 for a throw whose value is the JSON, because a peer
   answers by running a program (an IDL accessor, a page's setter or function) and a page's `try`/`catch` around
   the operation must see its throw. It is a parameter, so a zone must say which completion it answers with.
   `body`/`body_len` are qjs_provide's byte side-channel: XHR §3.5.6's fetch and §7.4 step 14's `{body, csp}` carry
   fetched bodies. An answer that is a number or a document name passes (NULL, 0). */
QJS_EXPORT void qjs_host_answer(unsigned req, const char *json, unsigned completion,
                                const char *body, unsigned body_len)
{
    DCHECK(g_begun, "an answer was provided to an engine that never ran");
    DCHECK(completion == ENGINE_COMPLETION_NORMAL || completion == ENGINE_COMPLETION_THROW,
           "the trusted zone answered a request with a completion type ECMA-262 6.2.4 does not have — an "
           "operation performed in another instance returns or throws, and nothing else crosses a call site");
    DCHECK(body != NULL || body_len == 0,
           "an answer's body arrived as a null pointer with a length — the pointer and the length describe ONE "
           "byte sequence, so an answer with no body at all passes both as nothing");
    {
        JSValue v = json ? JS_ParseJSON(g_ctx, json, strlen(json), "<host-answer>") : JS_UNDEFINED;
        /* A malformed answer is the host's bug; delivering the exception would surface as the document throwing. */
        DCHECK(!JS_IsException(v), "the host answered a synchronous request with text that is not JSON — the "
                                   "answer crosses as JSON so that it carries its type, and a bare string is "
                                   "not one (it is the JSON text `\"...\"`)");
        /* The body, if this answer has one. `body == NULL` says the answer carries no bytes (a number, a name, a
           completion), so the write happens where the host said there were bytes, and `fetch_reply_set_body`
           asserts the JSON did not also carry a decoded one. */
        if (body && JS_IsObject(v))
            fetch_reply_set_body(g_ctx, v, (const uint8_t *)body, (size_t)body_len);
        else
            DCHECK(body == NULL,
                   "the host answered with BYTES beside something that is not a record — an answer's body is a "
                   "field of the answer, so bytes arriving next to a number, a string or a `null` name a "
                   "record the host did not build");
        /* Routed to one call site by id, never broadcast as a fetched body is, because the answer was computed
           under the asking flow's world; a zero return means that flow is gone and nobody waits. No timeline is
           named: this zone computed the value itself, so there is one of it. */
        engine_host_answer(g_ctx, (uint32_t)req, NULL, v, (int)completion, ENGINE_ANSWER_HOST);
        JS_FreeValue(g_ctx, v);
    }
}

/* The trusted zone refusing to perform a synchronous request: `qjs_decline` refuses an address, this a rendezvous,
 * keyed by the id the engine put on its `qjs_host_requests` line. It is not a `completion` on qjs_host_answer:
 * ECMA-262 6.2.4's return and throw are completions of a call, and a refusal is the call not happening, so widening
 * that enum would hand `engine_host_take` a value the asking machine consumes. `reason` is the zone's own words, in
 * the vocabulary of a blocked scheme or private-network load, since only the rule's applier knows whether a
 * per-origin widening would let the request fire. A zero return is the asking flow having gone, as for
 * `qjs_host_answer`. */
QJS_EXPORT int qjs_host_decline(unsigned req, const char *reason)
{
    DCHECK(g_begun, "a request was refused to an engine that never ran");
    DCHECK(reason != NULL && *reason != '\0',
           "the trusted zone refused a synchronous request with no REASON — the flow parked on it will not "
           "drain this session, and a reader looking at a frontier that stops has nothing else to read");
    return engine_host_decline(g_ctx, (uint32_t)req, reason);
}

/* The return path for a notice. `record` is the notice verbatim as the emitting instance wrote it
   (`qjs_host_notices`), routed here because this instance holds the document it names; `sender_origin` is that
   instance's serialized origin, which only the trusted zone may state (SECURITY.md), since an engine-supplied
   origin would defeat every `event.origin` check. The delivery becomes a flow on the one frontier, ordered,
   preemptible and parkable; nothing runs inside this call. */
QJS_EXPORT void qjs_route(const char *record, const char *sender_origin)
{
    DCHECK(g_begun, "a record was routed to an engine whose frontier was never seeded — there is no scheduler to "
                    "run the delivery, so it would be dropped");
    engine_route(g_ctx, record, sender_origin);
}

/* The third record on that line, which seeds nothing: a peer saying one of its worlds is gone, so the COW segment
 * this instance materialized for it can go. It carries no target document (the sender does not track which peers a
 * flow reached, and releasing a world with no segment is a no-op, so the zone broadcasts it to every instance but
 * the sender) and no sender origin (no page code runs). `world` is world_serialize's grammar, verbatim. */
QJS_EXPORT void qjs_world_gone(const char *world)
{
    DCHECK(g_begun, "a world's death was announced to an engine whose frontier was never seeded — a foreign "
                    "segment is materialized by a delivery or an operation, and neither can have arrived");
    engine_world_gone(g_ctx, world);
}

/* The cross-instance seam's other direction: a peer asking this instance to perform an operation. `qjs_route` is a
 * one-way delivery and `qjs_host_answer` an answer the zone computed itself. `record` is the operation verbatim as
 * the asking instance wrote it (the text after the id on its `qjs_host_requests` line); `token` is the zone's
 * rendezvous, opaque to both engines, since two peers may hold the same request id.
 * Nothing runs inside this call: a peer answers by running a program, so the operation becomes a program of every
 * live timeline of the named document on the one frontier. Each completion leaves through `qjs_host_notices` as
 * `remoteop.answer<TAB><token><TAB><world><TAB><completion>`, one per timeline, since that document's state is its
 * flows; `<world>` distinguishes N true answers from one answer relayed N times. */
QJS_EXPORT void qjs_perform(const char *token, const char *record)
{
    DCHECK(g_begun, "a cross-agent operation was asked of an engine whose frontier was never seeded — there is "
                    "no timeline to perform it in and no scheduler to run the program that answers it");
    engine_perform(g_ctx, token, record);
}

/* The peer's completion coming back, in remote_object.c's grammar rather than JSON, so it is a second entry beside
 * qjs_host_answer: a zone's answer is a data record (`{body, csp}`), and a peer's may be an object, which crosses
 * as a name into the peer's namespace. The decode happens here, inside the asking instance; the zone routes the
 * text unread. `req` is this instance's request id, recovered by the zone from its token. `world` is which of the
 * answering document's timelines produced this completion, relayed off the `remoteop.answer` notice, so this entry
 * can tell a second timeline (a fork the asking flow owes) from one timeline's answer delivered twice. */
QJS_EXPORT void qjs_host_answer_remote(unsigned req, const char *world, const char *completion)
{
    int type = ENGINE_COMPLETION_NORMAL;
    JSValue v;

    DCHECK(g_begun, "a peer's completion was delivered to an engine that never ran");
    DCHECK(completion != NULL && *completion,
           "a peer's completion arrived with no text — a completion record carries its TYPE in front of its "
           "value, so an empty one is not `undefined`, it is a relay that lost the answer");
    DCHECK(world != NULL && *world,
           "a peer's completion arrived naming no TIMELINE — the answering instance writes the world of the "
           "flow that ran the program on every answer it emits, so an empty one is a relay that dropped the "
           "field, and this instance can then no longer tell the peer's other timelines from a duplicate");
    v = remote_completion_decode(g_ctx, completion, &type);
    /* Routed to one call site by id, as qjs_host_answer's is: the operation ran under the asking flow's world. A
       zero return means that flow is gone. */
    engine_host_answer(g_ctx, (uint32_t)req, world, v, type, ENGINE_ANSWER_PEER);
    JS_FreeValue(g_ctx, v);
}

QJS_EXPORT double qjs_top_weight(void)
{
    DCHECK(g_begun, "qjs_top_weight was asked of an engine whose frontier was never seeded");
    return engine_top_weight();
}

/* The value yield floor: the runner-up engine's weight, so the running flow yields the moment it is outranked
   across documents. A NaN would make every comparison false, so the top flow would never yield and the host's
   Level-1 interleave would stop silently. */
QJS_EXPORT void qjs_set_yield_floor(double floor)
{
    DCHECK(g_begun, "a yield floor was set on an engine whose frontier was never seeded — there is no flow to "
                    "rank against it");
    DCHECK(!(floor != floor), "the host set a yield floor of NaN — every weight comparison against it is false, "
                              "so the running flow is never outranked and the Level-1 interleave silently stops");
    engine_set_yield_floor(floor);
}

/* RAM pressure, from the one zone that can see it: the host holds every live engine and the summed working set,
   so the decision is the host's and the timing the engine's. The park is taken at the next step boundary with no
   flow switched in, when every flow's state is a snapshot; that step answers DONE and the residue leaves in the
   result document's `_park`, for the host to store in IndexedDB and hand to a later qjs_begin. Asking is not
   parking: the engine runs until that boundary. */
QJS_EXPORT void qjs_request_park(void)
{
    DCHECK(g_begun, "a park was requested of an engine whose frontier was never seeded — there is nothing to "
                    "write out, and the host would store an empty residue over whatever it had");
    engine_request_park();
}

/* A host instrument's program, asked of every live timeline: how a driver obtains a JS value computed in the
   analysed document's realm with this engine's unknowns on their concrete examples (solver/engine.h's
   engine_request_dump states the contract). Asking is not running: this is called between steps, with the slice
   closed, and preempt_hook aborts if the scheduler's policy is consulted outside a slice, so the scheduler
   evaluates the ask, which also makes each answer a fact about a timeline. `program` is a NUL-terminated string,
   unlike byte-carrying entries, because it is the zone's own text and a length would restate the terminator. A
   production host does not call this: the program runs in the analysed realm and can write there, so a zone
   reporting findings would be reporting on a document it changed. */
QJS_EXPORT void qjs_request_dump(const char *program)
{
    DCHECK(g_begun, "a dump was asked of an engine whose frontier was never seeded — there is no timeline to "
                    "run the program in, and the register the driver then reads reports a document that "
                    "produced nothing rather than a question nobody was there to answer");
    DCHECK(program != NULL && *program,
           "a dump was asked with no program — the ABI's string parameters cross as addresses and address "
           "zero converts to the EMPTY STRING, so a caller that composed nothing arrives here looking exactly "
           "like one that asked for an empty program, and every live flow would be handed a row to compile");
    engine_request_dump(program);
}


/* Paint every world this run mints (solver/engine.h states the population). It renders nothing, answers nothing,
   chooses no flow and switches nothing in. A host asks only between steps, so the arms this engine forks, runs and
   finishes inside one step are unreachable to any number of asks at the boundary; this standing mode returns the
   thread once per timeline that ends, and the host renders on each. The mode is one-way (flow.h's
   `flow_paint_every_world` says why: a set of world-named images must be a document's complete set of worlds). A
   run that asks for no picture reaches none of this. */
QJS_EXPORT void qjs_request_paint_every_world(void)
{
    DCHECK(g_begun, "an image of every world this run mints was asked of an engine whose frontier was never "
                    "seeded — the mode would stand and the walk that covers the members already standing has "
                    "none to walk, so the boot flow itself, which is the only member holding a document, would "
                    "be the one world such a run never photographed");
    engine_request_paint_every_world();
}

/* The answers, drained: one `<world><TAB><json>` record per timeline that ran the program, newline separated, ""
   when there are none. This file owns the pointer, as for qjs_result. It is not part of `qjs_result`: the result
   document is the product's findings, and an instrument's artifact folded into it would be a measurement posing as
   a finding (testing/render_diff.js refuses the same). */
QJS_EXPORT const char *qjs_dumps(void)
{
    DCHECK(g_begun, "the dump register was read from an engine that never ran");
    return engine_take_dumps();
}

/* Stream what is known so far: the same result document qjs_result returns, on the @RESULT line the smoke entry
   uses, so a long analysis reports as it goes. It touches no flow and drains nothing. The document is not a pure
   function of the frontier: composing it weighs the frontier (flow_wfq_census walks every member with flow_weight,
   then flow_best walks them again), and solver/flow.h's FLOW_SCANS counts that work, so two documents composed at
   one boundary differ in exactly four lifetime counts of `_wfq`: scanCensusRuns +1, scanCensusWeights +members,
   scanOtherRuns +1, scanOtherWeights +members (flow_wfq_census asserts the flow_best delta equals `g_flows_n`).
   Raising them is the cost of reporting, which `scanCensusWeights` against `scanNextWeights` prices; reading the
   counters before the walk would only make the figure one sample stale. `scanOther*` is shared with flow_best's
   other callers and the pager, so a streaming host inflates it by one frontier per call and nothing subtracts that.
   Every other `_wfq` row and every other field is composed from state this entry does not touch, so a checker of
   this contract exempts these four rows by name and holds the rest to equality. */
QJS_EXPORT void qjs_emit_partial(void)
{
    char *js;
    DCHECK(g_begun, "qjs_emit_partial was asked of an engine that never ran");
    /* The document is this line's to free: it is composed per call on the host's cadence and grows with the
       finding set, and a leaked copy would count in HEAPU8.length, which every reply reports as
       `workingSetBytes`, so the pool would page out the most productive instance first. */
    js = result_json(g_ctx);
    /* The same always-fatal allocation failure as qjs_result's, asserted rather than left to the printf: `%s` over
       a null pointer is undefined, and the host's JSON.parse would reject the line. */
    CHECK(js != NULL, "a partial result document could not be composed — the findings this engine has "
                      "accumulated so far are dropped, and the merge that would have surfaced them is skipped");
    printf("@RESULT %s\n", js);
    free(js);
    fflush(stdout);
}
