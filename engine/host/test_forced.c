/* End-to-end proof of the rebuilt frame-agnostic forced-execution core: a single concolic branch must explore
 * BOTH arms via the dispatch loop, with the fork expressed purely as a decision-vector sibling (no OP_if
 * rewind, no frame snapshot). Runs standalone against clean upstream quickjs + the new solver components. */
#include "quickjs.h"
#include "check.h"
#include "solver/concolic.h"
#include "solver/decide.h"
#include "solver/flow.h"
#include "solver/world.h"
#include "core/crypto/secure_hash.h"
#include "core/xml/xml_char.h"   /* XML §2.2/§2.3[3]/§2.11 — the layer every XML production reads through */
#include "core/frame/csp_source_list.h"
#include "core/frame/navigable.h"
#include "core/timing/event_loop.h"
#include "core/timing/timer.h"
#include "core/frame/window.h"
#include "core/frame/window_proxy.h"
#include "core/frame/window_message.h"   /* §9.3.3's two attacker sources, spelled once by their owner */
#include "core/frame/remote_object.h"
#include "core/html/html_iframe.h"
#include "core/html/html_parse.h"   /* the ONE place a Document is parsed — that header owns the token bytes */
#include "solver/engine.h"
#include "solver/cow.h"
#include "core/loader/document_scripts.h"
#include "core/frame/navigator.h"
#include "core/frame/screen.h"
#include "core/frame/viewport.h"
#include "core/frame/visual_viewport.h"
#include "core/dom/abort.h"
#include "core/dom/observable.h"
#include "core/html/unhandled_rejection.h"
#include "core/css/media_query_list.h"
#include "core/css/css_at_rule_prelude.h"
#include "core/css/css_property_syntax.h"
#include "core/css/css_syntax_match.h"
#include "core/rendering/animation_frame.h"
#include "core/rendering/page_reveal.h"
#include "core/rendering/rendering.h"
#include "core/html/trusted_types.h"
#include "core/dom/node.h"
#include "core/dom/element.h"
#include "core/geometry/dom_rect.h"
#include "core/geometry/dom_rect_list.h"
#include "core/frame/history.h"
#include "core/frame/navigation.h"
#include "core/frame/navigation_history_entry.h"
#include "core/frame/session_history.h"
#include "core/frame/location.h"
#include "core/idl_args.h"
#include "core/idl_async_iter.h"
#include "core/dom/document.h"
#include "core/events/event.h"
#include "core/events/error_event.h"
#include "core/events/message_event.h"
#include "core/events/report_exception.h"
#include "core/events/message_port.h"
#include "core/frame/policy_container.h"
#include "core/events/event_target.h"
#include "core/platform.h"
#include "core/realm.h"
#include "core/fetch/response.h"
#include "core/fetch/request.h"
#include "core/url/url.h"
#include "core/url/url_search_params.h"
#include "core/html/form_data.h"
#include "core/file/blob.h"
#include "core/file/file_reader.h"
#include "core/streams/readable_stream.h"
#include "core/streams/queuing_strategy.h"
#include "core/streams/writable_stream.h"
#include "core/streams/transform_stream.h"
#include "core/encoding/encoding.h"
#include "core/encoding/text_stream.h"
#include "core/fetch/headers.h"
#include "core/fetch/fetch.h"
#include "core/indexeddb/idb_connection.h"
#include "core/indexeddb/idb_database.h"
#include "core/indexeddb/idb_key.h"
#include "core/indexeddb/idb_key_path.h"
#include "core/indexeddb/idb_key_range.h"
#include "core/indexeddb/idb_object_store.h"
#include "core/indexeddb/idb_transaction.h"
#include "core/indexeddb/idb_upgrade_abort.h"
#include "solver/endpoint.h"
#include "solver/result.h"
#include "solver/solve.h"
#include "solver/cold.h"      /* the cross-session tier: this host's residue, and what a resume rebuilt */
#include "solver/dom_cow.h"   /* dom_attr_capture — the DOM write host-edge records into the per-flow DOM delta */
#include "solver/attr_shadow.h"   /* the (element, slot) taint shadow — freed with the frontier at teardown */
#include <lexbor/html/html.h>
#include <lexbor/dom/dom.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "core/dom/node_interface.h"   /* the ONE place a Document is made — see that header */

/* THERE IS NO `eval` STAND-IN HERE, AND ITS ABSENCE IS THE POINT. This fixture used to install a C function
   named `eval` over the intrinsic, which funnelled its argument into solve_eval_sink and returned undefined —
   so the eval sink's every probe and every breakout was measured against a function that EVALUATED NOTHING,
   and the fixture could not have caught the fact that no production call site existed. Worse, shadowing the
   global made `eval(x)` fail 13.3.6.1's identity test against %eval%, so the rows named for direct eval were
   never running one. The engine announces the real 19.2.1 and 20.2.1.1.1 now (JS_SetEvalSinkHook, registered
   by solve_init), so these rows drive the intrinsic and the sink's own evaluation is what fires them.
   setInnerHTML and setLocation stay stand-ins because what they stand in for is a HOST operation this fixture
   does not perform; `eval` was never in that class. */
/* the innerHTML host-edge: an HTML-context sink (setInnerHTML(x) stands in for el.innerHTML = x). */
static JSValue js_html_sink(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc > 0) solve_html_sink(ctx, argv[0]);
    return JS_UNDEFINED;
}
/* the location host-edge: a URL-context sink (setLocation(x) stands in for location = x / el.href = x). */
static JSValue js_url_sink(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc > 0) solve_url_sink(ctx, argv[0]);
    return JS_UNDEFINED;
}

/* the script-load host-edge: a lazy chunk / injected <script> / import(). Forced exec reaches it behind a
   branch; the loaded JS becomes more code in THIS flow (engine_queue_script), forking through the one BFS. In
   reality safeFetch fetches the body; here a mock chunk server returns it by URL. */
static JSValue js_load_script(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc > 0) {
        const char *url = JS_ToCString(ctx, argv[0]);
        if (url) {
            /* THE FIXTURE'S OWN DOCUMENT — a chunk this host mints belongs to the document that loaded it. */
            if (strstr(url, "admin")) engine_queue_script(world_local_doc(), "fetch('/api/admin/audit-log');");   /* chunk-only endpoint */
            /* A chunk that THROWS is what a page error IS — the report must name the capability. A DOMException
               is the most common throw in a DOM engine and keeps its name and message behind prototype
               accessors, so an own-property reader calls it "an object with no own name/message". */
            if (strstr(url, "cethrow")) engine_queue_script(world_local_doc(), "customElements.define('nohyphen', class {});");
            JS_FreeCString(ctx, url);
        }
    }
    return JS_UNDEFINED;
}

/* The fixture document's <body>, mutated by the DOM sink below so a DOM write is per-flow TIME-TRAVEL state:
   two forked flows write the same attribute differently and each reads back ITS OWN value. */
static lxb_dom_element_t *g_body = NULL;

/* the DOM WRITE host-edge (setBodyAttr stands in for el.setAttribute): capture the baseline into the running
   flow's DOM delta (dom_attr_capture), THEN mutate the shared Lexbor tree — so the write reverts on context-
   switch and the document a flow sees is per-flow. */
static JSValue js_set_body_attr(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 2 || !g_body) return JS_UNDEFINED;
    const char *name = JS_ToCString(ctx, argv[0]);
    const char *val = JS_ToCString(ctx, argv[1]);
    /* mutate ONLY through the chokepoint — capture-then-mutate atomically, never raw Lexbor + separate capture */
    if (name && val) dom_cow_set_attribute(g_body, name, val, strlen(val), JS_UNDEFINED);
    if (name) JS_FreeCString(ctx, name);
    if (val) JS_FreeCString(ctx, val);
    return JS_UNDEFINED;
}
/* the DOM READ host-edge (getBodyAttr stands in for el.getAttribute): reads the CURRENT flow's view of the
   attribute — the swap makes this per-flow. */
static JSValue js_get_body_attr(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1 || !g_body) return JS_NULL;
    const char *name = JS_ToCString(ctx, argv[0]);
    JSValue r = JS_NULL;
    if (name) {
        size_t vl = 0;
        const lxb_char_t *v = lxb_dom_element_get_attribute(g_body, (const lxb_char_t *)name, strlen(name), &vl);
        r = v ? JS_NewStringLen(ctx, (const char *)v, vl) : JS_NULL;
        JS_FreeCString(ctx, name);
    }
    return r;
}

/* the DOM NODE-INSERT host-edge (appendChild stands in for body.appendChild(el)): create a <span data-mark=..>
   and attach it through the insert chokepoint, so the appended subtree is per-flow TIME-TRAVEL state (tree
   structure, not just attributes). The mark attribute rides the inserted subtree — reverting the insertion
   removes the whole node, so it needs no separate capture. */
static JSValue js_append_child(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic) {
    (void)this_val; (void)magic;
    if (argc < 1 || !g_body) return JS_UNDEFINED;
    const char *mark = JS_ToCString(ctx, argv[0]);
    if (mark) {
        lxb_dom_document_t *doc = lxb_dom_interface_node(g_body)->owner_document;
        lxb_dom_element_t *span = lxb_dom_document_create_element(doc, (const lxb_char_t *)"span", 4, NULL);
        if (span) {
            lxb_dom_element_set_attribute(span, (const lxb_char_t *)"data-mark", 9,
                                          (const lxb_char_t *)mark, strlen(mark));   /* part of the inserted subtree */
            dom_cow_append_child(lxb_dom_interface_node(g_body), lxb_dom_interface_node(span));   /* chokepoint */
        }
        JS_FreeCString(ctx, mark);
    }
    return JS_UNDEFINED;
}
/* the DOM READ host-edge: the LAST child's data-mark — the CURRENT flow's view of the tree (per-flow). */
static JSValue js_last_child_mark(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    if (!g_body) return JS_NULL;
    lxb_dom_node_t *last = lxb_dom_node_last_child(lxb_dom_interface_node(g_body));
    if (!last) return JS_NULL;
    size_t vl = 0;
    const lxb_char_t *v = lxb_dom_element_get_attribute(lxb_dom_interface_element(last),
                                                        (const lxb_char_t *)"data-mark", 9, &vl);
    return v ? JS_NewStringLen(ctx, (const char *)v, vl) : JS_NULL;
}

/* THE TRUSTED HOST'S HALF OF A FETCH, which this fixture had never played. The Fetch component does not fetch —
   SECURITY.md puts every byte behind the trusted chokepoint — so it parks the URL and the host supplies the
   bytes. With no host doing that, a flow stopped at its first request and the continuation that reads the reply
   never ran; the run simply ended there.
   The reply is the same JSON every time, because what this fixture tests is the PATH (request parked, reply
   delivered, continuation resumed), not what any particular endpoint returns. Real replies are the live
   harness's business. */
static int hostreq_answer_all(JSContext *ctx);   /* the SYNCHRONOUS half — see the machine below */

/* THE PEER THIS FIXTURE STANDS IN FOR — declared here and defined beside the park moment it belongs to, which
   is the one thing that decides when the question is asked. */
static int  fixture_cold_moment(void);
static void fixture_ask_remote_op(JSContext *ctx);

static int fixture_provide(JSContext *ctx) {
    const char *urls = engine_pending_fetches();
    int filled = 0;
    UrlRecord base;

    /* THE HOST RESOLVES THE REQUEST'S ADDRESS, which is what makes the reply's URL list a list of URLs. A page
       fetches `/api/admin`; §4.1 gives the response a clone of the REQUEST's URL list, and `response.url` is
       the last item of that list SERIALIZED — so a relative reference in it is not a URL and there is nothing
       to serialize. The extension's trusted zone does exactly this (`new URL(u, sourceUrl)`); this fixture
       stands in for it and resolves against the same address it installed the Document at. */
    url_record_init(&base);
    CHECK(url_parse(&base, "https://x.test/p", strlen("https://x.test/p"), NULL),
          "the fixture could not parse its own document address");
    while (*urls) {
        const char *nl = strchr(urls, '\n');
        size_t len = nl ? (size_t)(nl - urls) : strlen(urls);
        char *one = malloc(len + 1);
        const char *method, *url;
        UrlRecord rec;
        char *abs;
        JSValue reply;
        bool ok;

        CHECK(one, "the fixture could not name the request it is answering");
        memcpy(one, urls, len); one[len] = 0;
        /* THE LINE IS `METHOD<TAB>URL` AND IT IS SPLIT BY THE ENGINE'S OWN SPLITTER — the reply seam is keyed on
           the pair, so a fixture that answered the whole line as an address would match nothing. */
        engine_pending_split(one, &method, &url);
        url_record_init(&rec);
        ok = url_parse(&rec, url, strlen(url), &base);
        DCHECK(ok, "the fixture was asked for a URL that will not parse even against its document's address");
        abs = ok ? url_serialize(&rec, /*exclude_fragment*/ false) : NULL;
        url_record_free(&rec);
        CHECK(abs, "the fixture could not serialize the URL it is answering");
        /* THE ONE REPLY RECORD every host delivers — the same shape the trusted zone stringifies as JSON. */
        /* `application/json` is what this fixture SERVES, stated by the host that serves it — the same
           position the trusted zone is in when it stamps `computedType` on a real reply (fetch.h). */
        /* AND IT ECHOES THE METHOD IT WAS ASKED FOR, which is what makes the pair seam OBSERVABLE from inside
           the page: a real echo endpoint answers a POST differently from a GET, and until this line every reply
           this fixture served was identical, so a GET's body arriving on a POST's promise looked exactly like
           the right answer. It rides a HEADER rather than the body deliberately — the body is 22 bytes and two
           probes read that length back as evidence that arrayBuffer() and bytes() are one byte sequence. */
        {
            HeaderList eh = { 0 };
            header_list_append(&eh, "x-echo-method", method);
            reply = fetch_reply_new(ctx, 200, "OK", &eh, "{\"region\":\"us-west-2\"}",
                                    strlen("{\"region\":\"us-west-2\"}"), (const char *const *)&abs, 1,
                                    "application/json");
            header_list_free(&eh);
        }
        filled += engine_provide(ctx, method, url, reply);
        JS_FreeValue(ctx, reply);
        free(abs);
        free(one);
        if (!nl) break;
        urls = nl + 1;
    }
    url_record_free(&base);
    if (fixture_cold_moment()) fixture_ask_remote_op(ctx);
    return filled + hostreq_answer_all(ctx);
}

/* THERE IS NO `fetch` STUB HERE ANY MORE. This file had one, and it was a SECOND implementation of the Fetch
   API: it read `init.method` with JS_GetPropertyStr from C — the exact drive-to-completion core/fetch/fetch.c
   was converted away from — and it knew nothing of `init.headers`, so the fixture could never have shown the
   endpoint's transport requirement no matter what the component did. The component is installed instead, which
   is what the shipped build runs. A request it issues parks on the host's reply and this fixture provides none,
   so a flow that fetches STALLS there; the endpoint is recorded before the park, which is why every existing
   probe still reads it. */

/* the "page": a real 2-<script> HTML document. Script 1 reads injected `state` into a config; script 2 (sharing
   globals) branches on it + does a baseline mutation (globalThis.n) first. Exercises: real Lexbor boot,
   cross-script concolic flow (fork on cfg.admin set by script 1), the moat (gated /api/admin), AND per-flow COW
   (both arms see n==1). */
static const char *HTML =
    "<!doctype html><html><head>"
    /* Author CSS whose SPECIFICITY and DOCUMENT ORDER disagree: the id rule is written FIRST and must still
       win. Order-only cascading answers `none` here, and every page puts its general rules last. */
    "<style>#cs1 { display: block; color: rgb(1, 2, 3) } div { display: none }</style>"
    /* CSS Properties and Values API 1 §3's `@property`, in a SHEET OF ITS OWN so its rules sit at fixed
       indices and the sheet above keeps index 0. Five bodies, because §3 decides five different things about
       one: every descriptor declared; NONE, which is what exercises §3.1's initial `"*"`, §3.2's initial `true`
       and §3.3's initial (the guaranteed-invalid value, i.e. a null `initialValue`); only an UNKNOWN
       descriptor, which §3 says is "invalid and ignored, but do not invalidate the @property rule" — so the
       rule must still be in `cssRules`; a `syntax` whose string §5.4.3 refuses (`inherit` is not a
       `<custom-ident>`), which §3.1 says leaves the descriptor ignored and therefore the initial standing; and
       §3.3's own cross-descriptor condition, which is the last one below and is the shape a shipping site
       actually declares — developer.mozilla.org ships
       `@property --switch-position{syntax:"<percentage>";inherits:false;initial-value:0}`, and it is the rule
       that used to abort the whole document at stage `create` with zero flows run. */
    "<style>"
    "@property --pfull { syntax: \"<percentage>\"; inherits: false; initial-value: 0% }"
    "@property --pbare { }"
    "@property --podd { not-a-descriptor: 3 }"
    "@property --pbad { syntax: \"inherit\" }"
    /* §3.3's CROSS-DESCRIPTOR CONDITION, which is the one thing about this interface a page can only see
       through `initialValue`: `if specified, the value of the initial-value descriptor must successfully parse
       according to the rule's syntax descriptor, or else the descriptor is invalid and ignored`. `0` is a
       `<number-token>` and CSS Values §5.5 makes a `<percentage>` a `<percentage-token>`, so the descriptor is
       ignored and §3.3's initial — the guaranteed-invalid value — stands. THIS IS THE MDN RULE EXACTLY:
       developer.mozilla.org ships `@property --switch-position{syntax:"<percentage>";inherits:false;
       initial-value:0}`, and the rule above (`--pfull`) is the same declaration written with the `%` that
       makes it parse — so the two differ by one byte and must differ in `initialValue`. */
    "@property --pmiss { syntax: \"<percentage>\"; inherits: false; initial-value: 0 }"
    "</style>"
    "</head><body><div id=cs1 style=\"margin-top: 4px\"></div><h1 id=dh>doch</h1>"
    /* ---- CSS Nesting Module Level 1 §3 "Nesting Style Rules" .. §6 "CSSOM" — ONE CONTIGUOUS BLOCK -------
       The sheet, the tree it selects and the three claims about it. A style rule with a nested rule in it used
       to ABORT the author cascade outright — the `@WHY` there said so in as many words — so the first claim is
       that a document holding one resolves at all.
       THE PARENT IS A SELECTOR LIST WHOSE MEMBERS DISAGREE ABOUT SPECIFICITY, which is the design of the
       fixture rather than decoration. §4 "Nesting Selector: the & selector": "The specificity of the nesting
       selector is equal to the largest specificity among the complex selectors in the parent style rule's
       selector list (identical to the behavior of :is())". The host matches through `.nstcls` and NEVER
       through `#nstid`, so `& .nstkid` carries (1,1,0) while a FLATTENED `.nstcls .nstkid` would carry
       (0,2,0) — and `.nstwrap .nstkid` is (0,2,0) written LATER, so it wins on CSS Cascade §6.1's Order of
       Appearance under the flattening and loses under the standard. §4's own worked example is this shape
       ("The text will be blue, rather than red"); a build that concatenates the parent instead of wrapping it
       in `:is()` answers `none` for `nstkid` here and is otherwise indistinguishable.
       ALL THREE §3.1 "Syntax" PRELUDE SHAPES ARE PRESENT, because all three reach this engine differently: the
       implied descendant (`.nstkid`, which the selector parser ACCEPTS and which is nevertheless relative),
       the non-relative nest-containing compound (`&.nstself`) and the leading-combinator relative selector
       (`> .nstchild`), the last two of which the selector parser refuses exactly as it refuses `!!!`. §6
       "CSSOM" requires all three to serialize ABSOLUTIZED, which is the second claim.
       The third is §3.3 "Nesting Other At-Rules" — a nested group rule, whose style rules are "nested style
       rules, with their nesting selector taking its definition from the nearest ancestor style rule" — and it
       is a SEPARATE endpoint because it is a separate capability: it fails on its own if the nested `@media`
       never becomes a rule, and reading that in the same token as the cascade would hide which broke. */
    "<style id=nstsheet>"
    "#nstid, .nstcls { color: rgb(9, 9, 9);"
    "  .nstkid { display: block }"
    "  &.nstself { display: inline-block }"
    "  > .nstchild { display: flex }"
    "  .nstdeep { .nstdeeper { display: table } }"
    "  @media all { .nstmedia { display: grid } }"
    "}"
    ".nstwrap .nstkid { display: list-item }"
    "</style>"
    "<div class=nstwrap><div id=nsthost class=\"nstcls nstself\">"
    "<span id=nstkid class=nstkid></span><span id=nstchild class=nstchild></span>"
    "<span id=nstmedia class=nstmedia></span>"
    "<span class=nstdeep><span id=nstdeeper class=nstdeeper></span></span>"
    "</div></div>"
    "<script>var nsh = document.getElementById('nstsheet').sheet;"
    "var nsr = nsh.cssRules[0]; var nsc = nsr.cssRules;"
    "var nsel = nsc.length + '|' + nsc[0].selectorText + '|' + nsc[1].selectorText + '|' +"
    " nsc[2].selectorText + '|' + nsc[3].selectorText + '|' + nsc[3].cssRules[0].selectorText;"
    "fetch('/api/cssnestsel?v=' + (nsel === '5|& .nstkid|&.nstself|& > .nstchild|& .nstdeep|& .nstdeeper'"
    " ? 'NESTSELOK' : 'NESTSELBAD:' + nsel));"
    "var nsd = function(i){ return getComputedStyle(document.getElementById(i)).display; };"
    "var nsv = nsd('nstkid') + '|' + nsd('nsthost') + '|' + nsd('nstchild') + '|' + nsd('nstdeeper');"
    "fetch('/api/cssnest?v=' + (nsv === 'block|inline-block|flex|table' ? 'NESTOK' : 'NESTBAD:' + nsv));"
    "fetch('/api/cssnestmedia?v=' + (nsd('nstmedia') === 'grid' ? 'NESTMEDIAOK' : 'NESTMEDIABAD:'"
    " + nsd('nstmedia')));</script>"
    "<script>var cfg = { admin: state.admin };"
    "var delObj = { k: 'keepVAL' };"   /* a shared BASELINE object; a forked flow will DELETE its k -> must revert per-flow */
    "var rx = { _f: 'base' };"   /* a reactive-framework style object: `flag` is an ACCESSOR backed by _f (Vue does exactly this) */
    "Object.defineProperty(rx, 'flag', { get: function(){ return this._f; }, set: function(v){ this._f = v; }, configurable: true });"
    /* HTML §4.12.1.1 "Processing model", the last step of "prepare the script element": "Otherwise, immediately
       execute the script element el, even if other scripts are already executing." This is the FIRST of the
       document's <script>s, so the program it injects belongs at a slot INSIDE the document's own sequence —
       between script 1 and script 2 — which is the one position the scheduler could not name while the
       document's scripts lived in a table separate from the flow's own rows: the slot arithmetic went NEGATIVE
       and a `CHECK` (fatal in release, and it fired on a real site) stopped the run there.
       THE PROBE IS THE ORDER AND NOT THE ABSENCE OF A CRASH. `__so` is appended to by each program in turn and
       the LAST <script> of the document reports it, so the two possible answers are different strings: `ABXC`
       is the injected row at its own position, and `ABC` is the same row taken to the TAIL of the sequence —
       which is where an engine that cannot address a slot inside the document's scripts would have to put it,
       and where it would then run after the program that reports. */
    "var __so = 'A';"
    "var soEl = document.createElement('script');"
    "soEl.textContent = \"__so += 'X';\";"
    "document.body.appendChild(soEl);"
    "__so += 'B';</script>"
    /* §6.1's FOUR ATTRIBUTES AND ITS SERIALIZATION, READ FROM THE PAGE. They are IDL getters, so the read has
       to be page code on the trampolined chain — a C activation has no flow base under it, and
       `JS_GetPropertyInternal` refuses to run a getter from one outright. Reading them here is also what a real
       page does, which is worth more than a C read would have been: `document.styleSheets`, §6.1.1's
       `cssRules`, §6.1.2's indexed getter and the rule objects are the whole route, and a member that is not on
       §6.1's prototype fails here rather than being read out of the record behind it.
       THE COMPARISON IS IN THE PAGE AND ONLY A TOKEN CROSSES THE URL, which is not a style choice: a fetch URL
       goes through the WHATWG query percent-encode set, which encodes SPACE, `"`, `<` and `>` — so a syntax
       string shipped as a query value arrives as `%3Cpercentage%3E`, and an assertion over that is an assertion
       about the encoder. The expected bytes are JS literals here, where nothing rewrites them, and the failing
       arm still carries the actual value for whoever has to read it.
       TWO FETCHES BECAUSE THEY ARE TWO INDEPENDENT CLAIMS — what the attributes ANSWER, and what §6.1's own
       serialization arm EMITS. A build could get the four right and the arm's order or spacing wrong, and one
       token over both would say only that one of two unrelated things is broken.
       NO FORK: every value is concrete (the CSSOM computed it), so each `===` runs rather than forking, and
       this adds two endpoint records and no arms. The joined string leads with the rule COUNT because an
       unknown descriptor and an unreadable syntax string are DESCRIPTOR failures and §3 says neither
       invalidates the rule — all four must be in `cssRules` — and it ends with the two syntaxes that reach
       §3.1's initial by two different routes. `type` is in it because §6.4.2's table is frozen and this
       interface is not in it, so the answer is 0, the same answer the two `@layer` rules give. */
    "<script>var pss = document.styleSheets[1].cssRules;"
    "var pv = pss.length + '|' + pss[0].name + '|' + pss[0].syntax + '|' + pss[0].inherits + '|' +"
    " pss[0].initialValue + '|' + pss[0].type + '|' + pss[1].syntax + '|' + pss[1].inherits + '|' +"
    " pss[1].initialValue + '|' + pss[2].syntax + '|' + pss[3].syntax + '|' + pss[4].initialValue;"
    "fetch('/api/cssprop?v=' + (pv === '5|--pfull|<percentage>|false|0%|0|*|true|null|*|*|null'"
    " ? 'CSSPROPOK' : 'CSSPROPBAD:' + pv));"
    "var pt = pss[0].cssText + '~' + pss[1].cssText;"
    "fetch('/api/csspropText?v=' + (pt === '@property --pfull { syntax: \"<percentage>\"; inherits: false;"
    " initial-value: 0%; }~@property --pbare { syntax: \"*\"; inherits: true; }'"
    " ? 'CSSTEXTOK' : 'CSSTEXTBAD:' + pt));</script>"
    "<script>"
    "fetch('/api/u?uid=' + state.id);"   /* concolic query param -> uid carries {state}.id */
    "if (navigator.userAgent.indexOf('Chrome') >= 0) { fetch('/api/uafork?v=chrome'); } else { fetch('/api/uafork?v=other'); }"   /* THE UA GATE: navigator.userAgent is concolic with a real Chrome example, so the string method computes on the example AND the comparison forks -> BOTH arms' endpoints are learned */
    "if (navigator.maxTouchPoints > 0) { fetch('/api/touch?v=touch'); } else { fetch('/api/touch?v=mouse'); }"
    "if (screen.width < 768) { fetch('/api/layout?v=mobile'); } else { fetch('/api/layout?v=desktop'); }"
    /* 13.12 Binary Bitwise Operators AND 13.9 Bitwise Shift Operators over UNKNOWN INPUT — the family that had
       no concolic semantics, so `h = (h << 5) ^ h | 0` (a minified bundle's string hash) reached the ToNumber
       boundary and aborted the whole document; three of thirty real signed-out product pages died there.
       `screen.width` is a concolic carrying a real example, so 6.1.6.1.16 NumberBitwiseOp / 6.1.6.1.9
       Number::leftShift / 6.1.6.1.11 Number::unsignedRightShift RUN on that example while the RESULT stays
       unknown — which the branch proves by forking. A collapse to NaN or to a bare number leaves one arm. */
    "var bwm = screen.width & 1023; if (bwm > 100) { fetch('/api/bwfork?v=bwwide'); } else { fetch('/api/bwfork?v=bwnarrow'); }"
    "fetch('/api/bwhash?h=' + (((screen.width << 5) ^ (screen.width >>> 3)) | 0));"
    /* THE OBJECT SIDE ON THE TRAMPOLINE, with a concolic on the other side. 13.15.3 step 3's ToNumeric(left)
       must NOT be performed at the operator's C boundary when the left is unknown (its ToNumeric is step 7's
       operation, which the hook answers), and step 4's ToPrimitive(right) is the PAGE's code with a loop in it,
       so it parks on the trampoline and resumes at the exact point. The result is still unknown, so the
       comparison forks — both arms are the claim. */
    "var bwo = { valueOf: function(){ var n = 0; for (var i = 0; i < 500; i++) { n += i; } return 12; } };"
    "fetch('/api/bwtramp?v=' + ((screen.width & bwo) !== 0 ? 'bwand' : 'bwzero'));"
    "var ac0c = new AbortController(); var ac0 = ac0c.signal;"
    "ac0.addEventListener({ toString: function(){ var n = 0; for (var i = 0; i < 500; i++) { n += i; } return 'abort'; } }, function(){ fetch('/api/idlcoerce?v=coerced'); });"   /* addEventListener's `type` is a Web IDL DOMString: the toString is the PAGE's code and has a loop in it, so the machine parks on step_tostring_run and resumes at the exact stage — and the listener is registered under the string that call RETURNED, which the abort below proves by firing it */
    "ac0c.abort();"
    "var ac = new AbortController(); ac.signal.addEventListener('abort', function(e){ fetch('/api/aborted?v=' + (e.isTrusted && e.type === 'abort' && e.target === ac.signal ? 'fired' : 'wrong')); }); ac.abort();"   /* the controller's signal is the REAL state machine: abort() reads [[Signal]] as an internal slot and fires `abort`, whose listener runs as its own task on this flow */
    "var tsig = AbortSignal.timeout({ valueOf: function(){ var n = 0; for (var i = 0; i < 2000; i++) { n += i; } return n; } });"   /* [EnforceRange] unsigned long long is ToNumber on the PAGE's object: the loop inside valueOf preempts, so the timeout machine must suspend and resume at the exact stage it parked on */
    "if (tsig.aborted) { fetch('/api/deadline?v=expired'); } else { fetch('/api/deadline?v=live'); }"   /* a timeout's aborted flag is UNKNOWN, so both arms run and the fallback path's endpoint is learned too */   /* THE responsive gate: a bundle routes, hosts assets and often bases its API on this, so both arms must be reached */   /* the desktop-vs-touch gate, the same shape over a numeric member */
    /* §13.15.3 ApplyStringOrNumericBinaryOperator STEP 1.c, ASKED OF THE EXAMPLE — "If leftPrimitive is a
       String or rightPrimitive is a String" — which is what decides whether `+` concatenates or ADDS. The
       concolic derivation had one arm, so `num + 1000000` carried the example "19201000000" where the code
       computed 1001920, and §@H publishes a computed value as an OBSERVED fact: every endpoint downstream of
       an arithmetic `+` named an address the bundle does not address. THREE SEPARATE CLAIMS.
       `addfork` is the one that must NOT change: the sum is still unknown, so the comparison forks and both
       arms' endpoints are learned — a numeric arm that collapsed to a bare number deletes one of them.
       `addnum` reads the example back through the path parameter endpoint.c aligns against the shape: `num`
       cancels, so the segment is `v1000000` under 6.1.6.1.7 Number::add ( x, y ) and `v19200998080` under a
       string concatenation, and the one exact match is the whole assertion.
       `addstr` is the regression that would cost the most — a String on the left and a concolic whose example
       is a NUMBER on the right is step 1.c's own case, and it is the `'/api/' + cfg.region` this surface is
       stated around; it reads `r1920` under both arms and must keep doing so. */
    "var addn = num + 1000000;"
    "if (addn > 0) { fetch('/api/addfork?v=addpos'); } else { fetch('/api/addfork?v=addneg'); }"
    "fetch('/api/addnum/v' + (addn - num));"
    "fetch('/api/addstr/r' + num);"
    /* THE REAL DOM, through document/Element/Node rather than a host-edge stand-in. Four things the tree
       components must get right and had no fixture for: interface members live on a PROTOTYPE (so two elements
       share one function object), `[LegacyNullToEmptyString] DOMString data` turns null into "" instead of the
       four characters `null`, `DOMString? textContent` set to null leaves NO child, and `nodeValue` is a NODE
       member that answers null on an element. */
    "var e1 = document.createElement('div'); var e2 = document.createElement('span');"
    "fetch('/api/protoid?v=' + (e1.getAttribute === e2.getAttribute ? 'shared' : 'percopy'));"
    "var tx = document.createTextNode('base'); e1.appendChild(tx); tx.data = null;"
    "fetch('/api/cdnull?v=' + (tx.data === '' ? 'empty' : 'wrong'));"
    "e1.textContent = null;"
    "fetch('/api/tcnull?v=' + (e1.childNodes.length === 0 ? 'nochild' : 'child'));"
    "fetch('/api/nodeval?v=' + (e1.nodeValue === null ? 'null' : 'wrong'));"
    "e1.textContent = 'tcSET'; fetch('/api/tcset?v=' + e1.textContent);"
    /* §4.4 THE NODE ALGORITHMS. Each is one spec sentence, and each answers with a token only the correct
       behaviour produces — every one of them was ABSENT, so a page doing any of this stopped there. */
    "fetch('/api/nodeconst?v=' + (document.body.nodeType === Node.ELEMENT_NODE ? 'isconst' : 'wrong'));"
    "fetch('/api/docnode?v=' + (document.nodeType === 9 && document.contains(document.body) ? 'isnode' : 'wrong'));"
    /* §4.4's baseURI IS THE DOCUMENT'S ADDRESS, so it is the address Location reports and not a different
       string. This fixture used to install the Document at "https://x.test" and Location at
       "https://x.test/p" — two answers to "where is this document", which no browser has and which only held
       because one caller passed each. One address now reaches both. */
    "fetch('/api/baseuri?v=' + (document.body.baseURI === 'https://x.test/p' ? 'base' : 'wrong'));"
    /* getRootNode's `composed` is a page GETTER, so the option read is a request the machine parks on and the
       loop inside it forces a suspend — the answer must still be the document. */
    "fetch('/api/rootnode?v=' + (document.body.getRootNode({ get composed(){ var n=0; for (var i=0;i<400;i++) n+=i; return true; } }) === document ? 'isroot' : 'wrong'));"
    "var c1 = document.createElement('p'); c1.setAttribute('k','v'); var c2 = c1.cloneNode(true);"
    "fetch('/api/equalnode?v=' + (c1.isEqualNode(c2) && !c1.isSameNode(c2) ? 'iseq' : 'wrong'));"
    /* THE WALK IS THE PAGE'S SIZE, so it is a MACHINE that yields at every pair. 300 nested nodes is 300
       suspension points inside one member — the answer being right afterwards is the whole claim, because a
       resume that lost its cursors would compare the wrong pair or re-walk from the top and never finish. */
    "var dp = document.createElement('div'), dq = dp;"
    "for (var di = 0; di < 300; di++) { var dn = document.createElement('span'); dn.setAttribute('k', 'v' + di);"
    " dq.appendChild(dn); dq = dn; }"
    "var dp2 = dp.cloneNode(true);"
    "var deep1 = dp.isEqualNode(dp2);"
    "dq.setAttribute('k', 'changed');"
    "var deep2 = dp.isEqualNode(dp2);"
    "fetch('/api/deepequal?v=' + (deep1 && !deep2 ? 'isdeep' : 'wrong'));"
    /* §4.4 cloneNode is a WALK and a COPY, and the oracles only ever compared a clone to its original with
       isEqualNode — which is itself a walk, so a matched pair of bugs in the two would agree with each other.
       These assert the copy's own properties instead: shallow takes no children, deep takes the whole subtree,
       attributes come with it, the copy is DETACHED and is not the original, and mutating the copy leaves the
       original alone. */
    "var cw = document.createElement('div'); cw.setAttribute('id', 'cwid');"
    "cw.innerHTML = '<p class=\"a\">x<b>y</b></p><i>z</i>';"
    "document.body.appendChild(cw);"
    "var csh = cw.cloneNode(); var cdp = cw.cloneNode(true);"
    "cdp.querySelector('b').setAttribute('k', 'v');"
    "fetch('/api/clone?sh=' + (csh.childNodes.length === 0 && csh.getAttribute('id') === 'cwid' ? 'shallow' : 'wrong')"
    " + '&dp=' + encodeURIComponent(cdp.innerHTML)"
    " + '&det=' + (cdp.parentNode === null && cdp !== cw && !cdp.isConnected ? 'detached' : 'wrong')"
    " + '&orig=' + (cw.querySelector('b').getAttribute('k') === null ? 'untouched' : 'shared'));"
    /* §4.10 a `<template>`'s children are on its CONTENT fragment, not under it, so a walk that follows
       first_child copies the template and none of its markup. Whatever this engine does, it should be stated by
       a test rather than discovered by a page. */
    "var ct = document.createElement('div');"
    "ct.innerHTML = '<template><b>tc</b></template>';"
    "var ctc = ct.cloneNode(true);"
    "fetch('/api/clonetpl?v=' + encodeURIComponent(ctc.innerHTML));"
    /* §4.10 a template has TWO child lists and both are real: the parser fills its content fragment, and
       `t.appendChild(x)` appends to the ELEMENT — only the parser and `t.content` reach the fragment. So a walk
       over a template must do the content AND then the ordinary children, which is why coming back from the
       content has to resume between the two. Nested, because one level proves nothing about the stack. */
    "var tt = document.createElement('div');"
    "tt.innerHTML = '<template><i>c1</i><template><s>c2</s></template></template>';"
    "var tte = tt.firstChild;"
    "tte.appendChild(document.createElement('u'));"
    "var ttc = tt.cloneNode(true);"
    "fetch('/api/tplboth?ser=' + encodeURIComponent(tt.innerHTML)"
    " + '&cl=' + encodeURIComponent(ttc.innerHTML)"
    " + '&kids=' + tte.childNodes.length + ':' + ttc.firstChild.childNodes.length"
    " + '&kn=' + (ttc.firstChild.firstChild ? ttc.firstChild.firstChild.nodeName : 'none'));"
    "var host = document.createElement('div');"
    "host.appendChild(document.createTextNode('A')); host.appendChild(document.createTextNode('B'));"
    "host.normalize();"
    "fetch('/api/normalize?v=' + (host.childNodes.length === 1 ? host.firstChild.data : 'wrong'));"
    "var ib = document.createElement('i'); host.insertBefore(ib, host.firstChild);"
    "fetch('/api/insertbefore?v=' + (host.firstChild === ib && ib.parentElement === host ? 'isfirst' : 'wrong'));"
    "fetch('/api/connected?v=' + (document.body.isConnected && !host.isConnected ? 'isconn' : 'wrong'));"
    "fetch('/api/position?v=' + ((document.body.compareDocumentPosition(c1) & Node.DOCUMENT_POSITION_DISCONNECTED) ? 'isdisc' : 'wrong'));"
    /* §4.4 compareDocumentPosition is FIVE walks behind one answer — two to the roots, two up the ancestor
       chains, and a pre-order walk of the whole shared tree when neither contains the other. Only the
       disconnected arm was asserted, so four of the five could have been anything. Each is a machine stage now,
       so a wrong resume in any of them is a wrong bit here. */
    "var pw = document.createElement('div'); document.body.appendChild(pw);"
    "var pa = document.createElement('p'); var pb = document.createElement('q');"
    "pw.appendChild(pa); pw.appendChild(pb);"
    "var pd = document.createElement('b'); pa.appendChild(pd);"
    "var P = Node;"
    "fetch('/api/posbits?anc=' + (pw.compareDocumentPosition(pd) === (P.DOCUMENT_POSITION_CONTAINED_BY | P.DOCUMENT_POSITION_FOLLOWING) ? 'contains' : 'wrong')"
    " + '&desc=' + (pd.compareDocumentPosition(pw) === (P.DOCUMENT_POSITION_CONTAINS | P.DOCUMENT_POSITION_PRECEDING) ? 'containedby' : 'wrong')"
    " + '&ord=' + (pa.compareDocumentPosition(pb) === P.DOCUMENT_POSITION_FOLLOWING"
    " && pb.compareDocumentPosition(pa) === P.DOCUMENT_POSITION_PRECEDING"
    " && pd.compareDocumentPosition(pb) === P.DOCUMENT_POSITION_FOLLOWING ? 'order' : 'wrong')"
    " + '&self=' + (pa.compareDocumentPosition(pa) === 0 ? 'zero' : 'wrong'));"
    /* §4.4 normalize's second loop: a RUN of adjacent Text siblings absorbed into the first, which is as long
       as the number of chunks a page appended. 40 of them, with empty ones interleaved so the drop arm runs
       too, and one non-Text node in the middle so the run has to STOP rather than swallow the tree. */
    "var nz = document.createElement('div');"
    "for (var nzi = 0; nzi < 40; nzi++) {"
      "nz.appendChild(document.createTextNode('' + (nzi % 10)));"
      "nz.appendChild(document.createTextNode('')); }"
    "nz.appendChild(document.createElement('hr'));"
    "for (var nzj = 0; nzj < 5; nzj++) nz.appendChild(document.createTextNode('z'));"
    "nz.normalize();"
    "fetch('/api/normrun?n=' + nz.childNodes.length + '&a=' + nz.firstChild.data + '&b=' + nz.lastChild.data);"
    /* The interface OBJECTS: `instanceof` up the whole chain, and a derived one inheriting Node's constants. */
    "fetch('/api/iface?v=' + (document.body instanceof Element && document.body instanceof Node &&"
    " document instanceof Document && tx instanceof Text && tx instanceof CharacterData &&"
    " Text.ELEMENT_NODE === 1 ? 'isiface' : 'wrong'));"
    /* §2.2 THE EVENT INTERFACE. EventInit is a DICTIONARY, so its members are property READS — the getter here
       is the page's code with a loop in it, and the conversion parks on that member and resumes. */
    "var ev = new Event('custom', { bubbles: true,"
    " get cancelable(){ var n = 0; for (var i = 0; i < 300; i++) { n += i; } return true; } });"
    "fetch('/api/event?v=' + (ev instanceof Event && ev.type === 'custom' && ev.bubbles && ev.cancelable &&"
    " !ev.isTrusted && ev.eventPhase === Event.NONE ? 'isevent' : 'wrong'));"
    /* preventDefault writes the CANCELED SLOT, which defaultPrevented and returnValue both read — they were a
       shared no-op over a public property the page could simply assign. */
    "ev.preventDefault();"
    "fetch('/api/evcancel?v=' + (ev.defaultPrevented && ev.returnValue === false ? 'iscancel' : 'wrong'));"
    /* §2.9 DISPATCH — SYNCHRONOUS, and its answer depends on what the listeners did. The first listener holds a
       LOOP, so the walk suspends inside it and resumes at the listener it was on; the second calls
       preventDefault, which is what dispatchEvent's false return reports. A job-enqueued dispatch would answer
       true here because nothing would have run yet. */
    "var dt = document.createElement('button'); var dhit = 0;"
    "dt.addEventListener('go', function(e){ var n = 0; for (var i = 0; i < 400; i++) { n += i; } dhit = n; });"
    "dt.addEventListener('go', function(e){ e.preventDefault(); });"
    "var dev = new Event('go', { cancelable: true });"
    "var dres = dt.dispatchEvent(dev);"
    "fetch('/api/dispatch?v=' + (dres === false && dhit === 79800 && dev.defaultPrevented &&"
    " dev.target === dt && dev.currentTarget === null && dev.eventPhase === 0 ? 'isdispatch' : 'wrong'));"
    /* stopImmediatePropagation ends the walk BETWEEN listeners, which is the only place it is observable. */
    "var st2 = document.createElement('b'); var sthit = 0;"
    "st2.addEventListener('s', function(e){ sthit += 1; e.stopImmediatePropagation(); });"
    "st2.addEventListener('s', function(e){ sthit += 10; });"
    "st2.dispatchEvent(new Event('s'));"
    "fetch('/api/stopimmediate?v=' + (sthit === 1 ? 'isstop' : 'wrong'));"
    /* HTML §8.1.7.2 EVENT HANDLER IDL ATTRIBUTES. The handler is NOT the listener: the slot keeps its POSITION
       while its handler changes underneath it, so `onh = a; addEventListener(h, b); onh = c` runs c then b. A
       run that registered the handler function itself would append it and get that backwards. */
    "var hh = document.createElement('u'); var order = '';"
    "hh.onclick = function(){ order += 'A'; };"
    "hh.addEventListener('click', function(){ order += 'B'; });"
    "hh.onclick = function(){ order += 'C'; };"
    "hh.dispatchEvent(new Event('click'));"
    "fetch('/api/handler?v=' + (order === 'CB' && typeof hh.onclick === 'function' ? 'ishandler' : 'wrong'));"
    /* Assigning null is §8.1.7's "deactivate": the slot goes, and the addEventListener listener stays. */
    "hh.onclick = null; order = ''; hh.dispatchEvent(new Event('click'));"
    "fetch('/api/handlernull?v=' + (order === 'B' && hh.onclick === null ? 'isnull' : 'wrong'));"
    /* The GLOBAL's set: window's IDL mixes in GlobalEventHandlers, and `onload` is how real code starts. */
    "globalThis.onerror = function(){ }; document.onreadystatechange = function(){ };"
    "fetch('/api/onglobal?v=' + (typeof globalThis.onerror === 'function' &&"
    " typeof document.onreadystatechange === 'function' && globalThis.onload === null ? 'isglobal' : 'wrong'));"
    /* HTML §4's ELEMENT-INTERFACE TABLE. The tag decides the interface, and the reflections belong to the
       interface that DECLARES them — `src` used to be on every element and `form.action` did not exist. */
    "var ia = document.createElement('a'); var isc = document.createElement('script');"
    "var ifo = document.createElement('form'); var iin = document.createElement('input');"
    "var idv = document.createElement('div'); var ixx = document.createElement('blink');"
    "fetch('/api/iface2?v=' + (ia instanceof HTMLAnchorElement && ia instanceof HTMLElement &&"
    " ia instanceof Element && ia instanceof Node && idv instanceof HTMLDivElement &&"
    " !(idv instanceof HTMLAnchorElement) && ixx instanceof HTMLUnknownElement ? 'isiface2' : 'wrong'));"
    /* The reflections are on their own interfaces now: a link has href, a div does not, and a div carrying an
       `src` property would be the old flat table answering for an attribute its IDL never declared. */
    "ia.href = '/api/reflected?v=isreflect'; ifo.action = '/api/formaction'; iin.name = 'q';"
    "isc.src = '/chunk/iface.js';"
    "fetch(ia.href);"
    "fetch('/api/reflect2?v=' + (ifo.getAttribute('action') === '/api/formaction' && iin.name === 'q' &&"
    " isc.getAttribute('src') === '/chunk/iface.js' && idv.href === undefined && idv.src === undefined"
    " ? 'isreflect2' : 'wrong'));"
    /* A BOOLEAN reflection is the attribute's PRESENCE, and it must be able to UNSET — which needs
       removeAttribute, which had no implementation at all. */
    "isc.async = true; iin.disabled = true; iin.disabled = false;"
    "fetch('/api/reflectbool?v=' + (isc.async === true && isc.getAttribute('async') === '' &&"
    " iin.disabled === false && iin.hasAttribute('disabled') === false ? 'isbool' : 'wrong'));"
    /* §2.6.1's URL REFLECTION: the getter encoding-parses and serializes against the element's NODE DOCUMENT
       and answers the ABSOLUTE URL, while the attribute keeps exactly what was written. A REFLECT_STRING row
       answers `/a.mp4` here and is INSTALLED, so the gap auditor scores it complete either way — this endpoint
       is the only thing in the tree that can tell the two apart. */
    "var iv = document.createElement('video');"
    "fetch('/api/refurlabsent?v=' + (iv.src === '' ? 'isempty' : 'wrong'));"
    "iv.src = '/a.mp4';"
    "fetch('/api/refurl?v=' + (iv.src === 'https://x.test/a.mp4' &&"
    " iv.getAttribute('src') === '/a.mp4' ? 'isabs' : 'wrong'));"
    /* A path-relative value resolves against the document's PATH, not against the origin alone. */
    "iv.setAttribute('src', 'b.mp4');"
    "fetch('/api/refurlrel?v=' + (iv.src === 'https://x.test/b.mp4' ? 'isrel' : 'wrong'));"
    /* Step 3: a value that does not parse reads back as itself — a page that wrote garbage reads garbage. */
    "iv.src = 'https://[!';"
    "fetch('/api/refurlbad?v=' + (iv.src === 'https://[!' ? 'israw' : 'wrong'));"
    /* `src` is HTMLMediaElement's ONE member and not a copy per subclass — the reflection put it on the
       prototype the IDL names, which the two deleted hand-written accessors also had to get right. */
    "fetch('/api/refurlproto?v=' + ("
    " Object.getOwnPropertyDescriptor(HTMLVideoElement.prototype, 'src') === undefined &&"
    " typeof Object.getOwnPropertyDescriptor(HTMLMediaElement.prototype, 'src').get === 'function'"
    " ? 'isone' : 'wrong'));"
    /* §2.6.1's `unsigned long` reflection, whose two fallbacks are DIFFERENT STEPS. `<td>` declares a range
       starting at 0 for rowspan and 1 for colspan, and a default of 1 for both — so an in-range 0 and an
       unparseable value give different answers on the same element, which one "fallback" number cannot. */
    "var itd = document.createElement('td');"
    "fetch('/api/ulabsent?v=' + (itd.colSpan === 1 && itd.rowSpan === 1 ? 'isdflt' : 'wrong'));"
    "itd.setAttribute('rowspan', '0'); itd.setAttribute('colspan', '0');"
    /* rowspan 0 is IN RANGE; colspan 0 is below its minimum and CLAMPS up, not to the default (they agree at
       1 here, so the discriminating pair is rowspan's 0-vs-1). */
    "fetch('/api/ulrange?v=' + (itd.rowSpan === 0 && itd.colSpan === 1 ? 'isrange' : 'wrong'));"
    "itd.setAttribute('rowspan', 'x');"
    "fetch('/api/uldflt?v=' + (itd.rowSpan === 1 ? 'isdflt2' : 'wrong'));"
    /* An overflowing digit run is ABOVE the maximum, not a parse error — the clamp, never the default. */
    "itd.setAttribute('rowspan', '99999999999999999999999');"
    "fetch('/api/uloverflow?v=' + (itd.rowSpan === 65534 ? 'isclamp' : 'wrong'));"
    /* THE SETTER DOES NOT CLAMP: "clamped to the range has no effect on the setter steps", so the attribute
       holds 5000 and only the read pulls it to 1000. A setter that clamped would store 1000 and the two
       assertions below would agree with each other and with a browser on neither. */
    "itd.colSpan = 5000;"
    "fetch('/api/ulset?v=' + (itd.getAttribute('colspan') === '5000' && itd.colSpan === 1000"
    " ? 'isnoclamp' : 'wrong'));"
    /* A value the IDL's own unsigned-long conversion leaves above 2147483647 is out of the setter's window, so
       the DEFAULT is written — not a wrap and not a clamp. */
    "itd.colSpan = 3000000000;"
    "fetch('/api/ulwindow?v=' + (itd.getAttribute('colspan') === '1' ? 'isdefaultwritten' : 'wrong'));"
    /* headingOffset has a range and NO default, so its fallback is the range's minimum. */
    "var ihd = document.createElement('div'); ihd.setAttribute('headingoffset', 'x');"
    "fetch('/api/ulmin?v=' + (ihd.headingOffset === 0 && ihd.headingReset === false ? 'ismin' : 'wrong'));"
    /* §3.2.2 click() is "fire a synthetic pointer event named click" — the SAME dispatch, so a handler wired
       with onclick sees an untrusted, cancelable, bubbling event and preventDefault reaches the caller. */
    "var ck = 0; var ckt = null; idv.onclick = function(e){ ck++; ckt = e; e.preventDefault(); };"
    "idv.click();"
    "fetch('/api/click?v=' + (ck === 1 && ckt.type === 'click' && ckt.isTrusted === false &&"
    " ckt.bubbles && ckt.cancelable && ckt.defaultPrevented && ckt.target === idv ? 'isclick' : 'wrong'));"
    /* CSSOM. getComputedStyle layers inline over the author cascade over the UA default over the property's
       initial value, all resolved from THIS flow's tree. */
    "var cs = document.getElementById('cs1'); var csc = getComputedStyle(cs);"
    "fetch('/api/cascade?v=' + (csc.display === 'block' ? 'isspec' : 'wrong'));"
    "fetch('/api/cssinline?v=' + (csc.marginTop === '4px' && cs.style.marginTop === '4px' &&"
    " cs.style.display === '' ? 'isinline' : 'wrong'));"
    /* The UA default reaches an element no rule names, and an undeclared property reads its INITIAL value. */
    "var csp = document.createElement('p'); document.body.appendChild(csp);"
    "fetch('/api/cssua?v=' + (getComputedStyle(csp).display === 'block' &&"
    " getComputedStyle(document.createElement('span')).display === 'inline' ? 'isua' : 'wrong'));"
    /* A USED VALUE THAT IS A JOINT FUNCTION OF TWO ENVIRONMENT FACTS — the shape a box with a border has, and
       the one that used to crash. CSS 2.1 §10.3.3 solves this `width: auto` against the INITIAL CONTAINING
       BLOCK and subtracts a border css-values §6 SNAPPED to a whole number of DEVICE PIXELS, so the number
       moves with both and its domain is ONE identity over the pair (solver/concolic.h) rather than either
       fact alone. There is no token to compare against: the value is a concolic, so what this asserts is the
       arithmetic's own asserts staying silent, and what it emits is the joint shape. */
    "var jw = document.createElement('p'); document.body.appendChild(jw);"
    "jw.style.setProperty('border-left-style', 'solid');"
    "jw.style.setProperty('border-left-width', '1px');"
    "fetch('/api/cssjoint?w=' + getComputedStyle(jw).width);"
    /* ---- css-fonts-4 §2.5's computed `font-size` and css-values-4 §6.1.1's `em`/`rem` --------------------
       EVERY VALUE COMPARED HERE IS ROOTED IN A DECLARED ABSOLUTE FONT SIZE, deliberately: an undeclared one is
       §2.5's `medium`, which IS the CSS_ENV_DEFAULT_FONT_SIZE fact, so its computed value is a CONCOLIC and a
       `===` against it would fork rather than decide — the same reason /api/cssjoint one probe up emits
       instead of comparing. Every arithmetic below is exact in binary64 (20 x 1.2 is 24.0, not 23.999…), so a
       serialization that round-trips is the only thing the equality depends on.
       §2.5's `Percentages:` line is "refer to parent element's font size" and §2.5's `<relative-size>` is
       "relative to the computed font-size of the parent element", so `150%` and `larger` are both a ratio of
       the PARENT's 20px — and so is a `1.5em` INSIDE `font-size`, which is the case §6.1.1's font-affecting
       rule decides and the one an implementation gets wrong by resolving against the element's own size. The
       same `2em` in a `margin-top` is NOT font-affecting and resolves against the element's OWN 20px, which is
       what makes the pair a test rather than two spellings of one number. */
    "var fsp = document.createElement('div'); document.body.appendChild(fsp);"
    "fsp.style.setProperty('font-size', '20px'); fsp.style.setProperty('margin-top', '2em');"
    "var fse = document.createElement('div'); fsp.appendChild(fse);"
    "fse.style.setProperty('font-size', '1.5em');"
    "var fsq = document.createElement('div'); fsp.appendChild(fsq);"
    "fsq.style.setProperty('font-size', '150%');"
    "var fsl = document.createElement('div'); fsp.appendChild(fsl);"
    "fsl.style.setProperty('font-size', 'larger');"
    "fetch('/api/fontsize?v=' + (getComputedStyle(fsp).fontSize === '20px' &&"
    " getComputedStyle(fse).fontSize === '30px' && getComputedStyle(fsq).fontSize === '30px' &&"
    " getComputedStyle(fsl).fontSize === '24px' && getComputedStyle(fsp).marginTop === '40px'"
    " ? 'isfs' : 'wrong'));"
    /* §6.1.1's `rem` is "the computed value of the em unit on the ROOT element", so the root's own font size is
       declared here for the same reason the parent's is above — an undeclared root font size is the default
       fact and the answer would carry its domain. */
    "document.documentElement.style.setProperty('font-size', '10px');"
    "var fsr = document.createElement('div'); document.body.appendChild(fsr);"
    "fsr.style.setProperty('margin-top', '2rem');"
    "fetch('/api/fontrem?v=' + (getComputedStyle(fsr).marginTop === '20px' ? 'isrem' : 'wrong'));"
    /* AND THE SELF-REFERENTIAL PAIR, LAST because it changes the root's font size out from under the probe
       above. §6.1.1: inside a font-affecting property on the element the unit REFERS TO, the base is the
       parent's metrics "or … the computed metrics corresponding to the initial values of the font and
       line-height properties, if the element has no parent" — so `html { font-size: 2rem }` is twice the
       INITIAL font size and not twice itself, and the root has no parent element. There is no token to compare
       against (the base is the default-font-size fact), and what this asserts is that the walk TERMINATED at
       all: resolving either unit against the element's own computed font-size here is not a wrong number, it
       is a definition of itself. */
    "document.documentElement.style.setProperty('font-size', '2rem');"
    "fetch('/api/fontroot?s=' + getComputedStyle(document.documentElement).fontSize);"
    /* element.style WRITES go through setAttribute's chokepoint, so an inline style time-travels like every
       other DOM write — and [SameObject] means the page gets the same declaration back each read. */
    "cs.style.color = 'red'; cs.style.setProperty('padding-left', '2px');"
    "fetch('/api/cssset?v=' + (cs.style.color === 'red' && cs.getAttribute('style').indexOf('padding-left') >= 0"
    " && cs.style === cs.style ? 'isset' : 'wrong'));"
    "cs.style.removeProperty('color');"
    "fetch('/api/cssdel?v=' + (cs.style.color === '' && cs.style.length >= 1 ? 'isdel' : 'wrong'));"
    /* A computed declaration is READ-ONLY, which the spec makes an error rather than a silent no-op. */
    "var csro = 'nothrow'; try { csc.setProperty('color', 'blue'); } catch (e) { csro = 'isro'; }"
    "fetch('/api/cssro?v=' + csro);"
    /* §8.4 THE FRAGMENT SERIALISER. innerHTML was WRITE-ONLY, so every read answered undefined — and undefined
       does not throw, it PROPAGATES: `wrap.innerHTML = head.innerHTML + row` builds the string "undefined…"
       and the page carries on, so the engine reported a surface assembled out of a value the page never had.
       outerHTML is the same serialiser over the element itself. */
    "var sz = document.createElement('section');"
    "sz.setAttribute('data-k', 'v');"
    "sz.innerHTML = '<p class=\\'q\\'>hi<br></p>';"
    "fetch('/api/serialize?in=' + encodeURIComponent(sz.innerHTML)"
    " + '&out=' + encodeURIComponent(sz.outerHTML));"
    /* §4.9 THE OTHER HTML-CONTEXT SINKS. insertAdjacentHTML and outerHTML= parse markup exactly as innerHTML
       does and were absent entirely — so a bundle using them had its DOM unbuilt AND its XSS invisible, which
       is the pair this engine exists to report. The four positions are one algorithm; what differs is where
       the fragment lands and, for the outside two, that it is parsed in the PARENT's context. */
    "var ia = document.createElement('div'); document.body.appendChild(ia);"
    "var iac = document.createElement('p'); ia.appendChild(iac);"
    "iac.insertAdjacentHTML('beforebegin', '<i>bb</i>');"
    "iac.insertAdjacentHTML('afterbegin', '<b>ab</b>');"
    "iac.insertAdjacentHTML('beforeend', '<u>be</u>');"
    "iac.insertAdjacentHTML('afterend', '<s>ae</s>');"
    "iac.insertAdjacentText('afterbegin', 'T');"
    "iac.insertAdjacentElement('beforeend', document.createElement('q'));"
    "fetch('/api/adjacent?v=' + encodeURIComponent(ia.innerHTML));"
    /* §13.4: the CONTEXT decides what survives. A `<td>` parsed against a `<tr>` is kept; the same markup
       parsed against a `<div>` is dropped by the tree builder, which is why the context is not a caller's
       choice. Only the row's own innerHTML can show it. */
    "var tb = document.createElement('table'); var tr = document.createElement('tr');"
    "tb.appendChild(tr); document.body.appendChild(tb);"
    "tr.insertAdjacentHTML('beforeend', '<td>cell</td>');"
    "fetch('/api/fragctx?v=' + encodeURIComponent(tr.innerHTML));"
    /* outerHTML= REPLACES the element in its parent, so the old one is gone and the new markup is in its place. */
    "var oh = document.createElement('div'); document.body.appendChild(oh);"
    "var ohk = document.createElement('span'); ohk.setAttribute('id','goneid'); oh.appendChild(ohk);"
    "ohk.outerHTML = '<h1>replaced</h1>';"
    "fetch('/api/outerset?v=' + encodeURIComponent(oh.innerHTML));"
    /* §13.2 A HEADING END TAG, which lexbor v2.4.0 mis-tokenised in FRAGMENT parsing only: `</h1>` lost its
       digit to a bogus comment and never closed the heading, so everything after it was nested INSIDE it. The
       document parse was clean, which is why nothing caught it until innerHTML could be READ. Both halves are
       asserted, because the version pin is what makes them true. */
    "var pz = document.createElement('div');"
    "pz.innerHTML = '<h1>a</h1>tail';"
    "fetch('/api/heading?v=' + encodeURIComponent(pz.innerHTML)"
    " + '&d=' + encodeURIComponent(document.querySelector('#dh').innerHTML));"
    /* §4.10 `<template>`'s CHILDREN ARE NOT UNDER IT. They live on its content fragment, whose node has no
       parent, so the serialiser's walk has to leave the tree it is on and come back — lexbor recurses there,
       and a machine whose C stack is gone at every suspension has to push instead. Nested, because one level
       of that push proves nothing about the stack. */
    "var tp = document.createElement('div');"
    "tp.innerHTML = '<template><i>x</i><template><b>y</b></template>z</template>after';"
    "fetch('/api/template?v=' + encodeURIComponent(tp.innerHTML));"
    /* A WALK THAT SUSPENDS MANY TIMES. One node per step is only correct if the resume comes back to the node
       it was on: a cursor that restarts duplicates the prefix, one that over-advances drops a node, and either
       shows up as the wrong bytes. 64 nested elements is 128 steps and the same number of suspensions, and the
       nesting is what makes an ascent bug visible — a flat list ascends once. */
    "var dp = document.createElement('div'); var dpc = dp;"
    "for (var dpi = 0; dpi < 64; dpi++) { var dpn = document.createElement('b'); dpc.appendChild(dpn); dpc = dpn; }"
    "dpc.appendChild(document.createTextNode('deep'));"
    "var dps = dp.innerHTML;"
    "fetch('/api/serdeep?o=' + (dps.split('<b>').length - 1) + '&c=' + (dps.split('</b>').length - 1)"
    " + '&t=' + (dps.indexOf('deep') > 0 ? 'once' : 'lost'));"
    /* §4.9: a position that is not one of the four is a SyntaxError, and an outside position with no element
       parent is a NoModificationAllowedError. Neither is a quiet no-op. */
    "var iaerr = 'wrong'; try { iac.insertAdjacentHTML('nowhere', 'x'); } catch (e) { iaerr = 'issyn'; }"
    "fetch('/api/adjbad?v=' + iaerr);"
    /* THE SINK HALF. Attacker input reaching insertAdjacentHTML is the same HTML-context breakout innerHTML
       reports — a sink that was invisible because the member did not exist. */
    "document.createElement('div').insertAdjacentHTML('beforeend', '<div>' + state.html2 + '</div>');"
    /* §4.2.7/§4.2.8 the ChildNode and ParentNode mixins. A bundle that builds its UI with append() and tears it
       down with remove() had NONE of it — the page's own call threw and every fetch behind that render never
       happened. A STRING argument becomes a Text node, which is the whole reason they take (Node or DOMString). */
    "var mx = document.createElement('ul'); document.body.appendChild(mx);"
    "var li1 = document.createElement('li'); var li2 = document.createElement('li');"
    "mx.append(li1, 'tail'); mx.prepend(li2);"
    /* §4.2.4's union, through the DECLARATION. `(Node or DOMString)...` is variadic, so EVERY argument is
       brand-checked and the non-Nodes are coerced before the body runs — and the coercion is the page's code
       with a loop in it, which used to be a JS_ToCStringLen from C with no flow base to park into. Nine
       arguments, because a fixed argument array is a cap and `ul.append(...items)` is how a list renders. */
    "var mv = document.createElement('ol'); document.body.appendChild(mv);"
    "var mvt = { toString: function(){ var n = 0; for (var i = 0; i < 60; i++) n += i; return 'n' + n; } };"
    "mv.append(document.createElement('li'), mvt, 'a', 'b', 'c', 'd', 'e', 'f', 'g');"
    "fetch('/api/variadic?v=' + encodeURIComponent(mv.innerHTML));"
    "li1.before(document.createElement('hr'));"
    "var mid = document.createElement('em'); li1.after(mid);"
    "fetch('/api/mixin?v=' + encodeURIComponent(mx.innerHTML));"
    "mid.replaceWith(document.createElement('b'));"
    "li2.remove();"
    "fetch('/api/mixin2?v=' + encodeURIComponent(mx.innerHTML));"
    "mx.replaceChildren(document.createElement('span'));"
    "fetch('/api/mixin3?v=' + encodeURIComponent(mx.innerHTML));"
    /* §4.2.10/§4.2.11: childNodes and children are LIVE, which is the difference a static array cannot express
       — read the length, append, read it again. querySelectorAll is the one that is genuinely STATIC, and all
       three are real interfaces now rather than Arrays, so `.map` is honestly absent as the spec has it. */
    "var lv = document.createElement('div'); document.body.appendChild(lv);"
    "lv.appendChild(document.createElement('i')); lv.appendChild(document.createTextNode('t'));"
    "var lvc = lv.childNodes; var lve = lv.children; var lvq = lv.querySelectorAll('i');"
    "var lvn0 = lvc.length + ':' + lve.length + ':' + lvq.length;"
    "lv.appendChild(document.createElement('b'));"
    "var lvn1 = lvc.length + ':' + lve.length + ':' + lvq.length;"
    "fetch('/api/live?v=' + (lvn0 === '2:1:1' && lvn1 === '3:2:1'"
    " && lv.childNodes === lvc && lv.children === lve && lvq instanceof NodeList"
    " && lve instanceof HTMLCollection && lvc[0] === lv.firstChild && typeof lvc.map === 'undefined'"
    " ? 'islive' : 'wrong'));"
    /* THE INDEX CACHE, and the two ways it can be wrong. A collection has nothing to cache the tree in, so
       `list[i]` is a walk of i and the loop every page writes over one is quadratic in the page's own markup;
       the cache is keyed on the tree version, exactly as Blink's CollectionIndexCache is on the document's.
       (1) SEQUENTIAL: 200 children read forwards then backwards — 400 reads that were 40,000 link steps, and
       a cursor that steps the wrong way or fails to move gives the wrong join. (2) INVALIDATION: insert at the
       FRONT between two reads. Every index shifts by one, the cached member is still a child of the same owner
       so no cheap check catches it, and only the version bump makes the second read right. */
    "var ic = document.createElement('div'); document.body.appendChild(ic);"
    "for (var ici = 0; ici < 200; ici++) {"
      "var icn = document.createElement('b'); icn.setAttribute('id', 'n' + ici); ic.appendChild(icn); }"
    "var icl = ic.children, icf = '', icb = '';"
    "for (var icj = 0; icj < icl.length; icj++) icf += icl[icj].getAttribute('id');"
    "for (var ick = icl.length - 1; ick >= 0; ick--) icb += icl[ick].getAttribute('id');"
    "var icv = icl[7].getAttribute('id');"
    "ic.insertBefore(document.createElement('b'), ic.firstChild);"
    "fetch('/api/idxcache?f=' + (icf.indexOf('n0n1n2') === 0 && icf.lastIndexOf('n199') === icf.length - 4"
    " ? 'fwd' : 'wrong') + '&b=' + (icb.indexOf('n199n198') === 0 ? 'back' : 'wrong')"
    " + '&i=' + (icv === 'n7' && icl[7].getAttribute('id') === 'n6' && icl.length === 201 ? 'shifted' : 'stale'));"
    /* §4.2.6 THE MIXIN IS ONE THING. Document had querySelector and querySelectorAll but NOT children,
       firstElementChild, lastElementChild or childElementCount — the IDL puts all of them on ParentNode, so a
       page reading `document.children` got undefined and took the branch behind it. And Document's
       querySelector ignored its receiver entirely, scoping every lookup to the global document's root, so a
       SCOPED lookup off an element that happened to reach document.c's copy searched the whole page. */
    "var pn = document.createElement('div'); document.body.appendChild(pn);"
    "pn.innerHTML = '<p id=\"pna\"></p>txt<span id=\"pnb\"></span>';"
    "fetch('/api/parentmixin?dc=' + (document.children.length === 1"
    " && document.firstElementChild === document.documentElement"
    " && document.childElementCount === 1 ? 'doc' : 'wrong')"
    " + '&el=' + (pn.children.length === 2 && pn.childNodes.length === 3"
    " && pn.firstElementChild.getAttribute('id') === 'pna'"
    " && pn.lastElementChild.getAttribute('id') === 'pnb' ? 'el' : 'wrong')"
    /* the scope is the RECEIVER: `pn` has no <body> under it, the document does. */
    " + '&sc=' + (pn.querySelector('body') === null && document.querySelector('body') === document.body"
    " && pn.querySelectorAll('p').length === 1 ? 'scoped' : 'wrong'));"
    /* ---- §4.2.6 moveBefore / §4.2.3 move — ONE CONTIGUOUS BLOCK, /api/movebefore* ------------------------
       THE POINT OF THE MEMBER IS THE STATE IT DOES NOT DESTROY, so the assertion that matters is the one a
       remove-then-insert `moveBefore` would fail while getting the tree shape right: a custom element that is
       MOVED gets `connectedMoveCallback` and does NOT get the disconnected/connected pair. HTML §4.13.2.1
       "Preserving custom element state when moved" is that behaviour, and a page that loses it silently loses
       whatever the callbacks reset — an observer, a tab index, an iframe's document.
       The three validity assertions are the ones where move's six steps DIVERGE from pre-insert's eleven:
       step 1 (one shadow-including root, which is what makes every disconnected↔connected move throw) has no
       pre-insert counterpart at all, and step 4 refuses a DocumentFragment and a DocumentType that pre-insert
       validity step 4 admits. */
    "var mbCe = [];"
    "customElements.define('x-moved', class extends HTMLElement {"
      "connectedCallback(){ mbCe.push('c'); }"
      "disconnectedCallback(){ mbCe.push('d'); }"
      "connectedMoveCallback(){ mbCe.push('m'); } });"
    "var mbA = document.createElement('div'); document.body.appendChild(mbA);"
    "var mbB = document.createElement('div'); var mbC = document.createElement('div');"
    "mbA.append(mbB, mbC);"
    "var mbR = mbA.moveBefore(mbC, mbB);"
    "var mbOrder = mbA.firstChild === mbC && mbA.lastChild === mbB && mbR === undefined;"
    /* §4.2.6 step 2: a reference child that IS the node becomes its next sibling, so this is a no-op. */
    "mbA.moveBefore(mbC, mbC); mbA.moveBefore(mbB, mbB);"
    "var mbSelf = mbA.firstChild === mbC && mbA.lastChild === mbB;"
    "fetch('/api/movebefore?v=' + (mbOrder && mbSelf ? 'ismoved' : 'wrong'));"
    "var mbThrew = '';"
    /* STEP 1, the step pre-insert does not have: a node with no parent is its own shadow-including root, so
       every disconnected→connected move is a HierarchyRequestError where the same appendChild succeeds. */
    "try { document.body.moveBefore(document.createElement('p'), null); mbThrew += 'none'; }"
      "catch (e) { mbThrew += e.name; }"
    /* …and a DocumentFragment is always its own root, so step 1 answers before step 4 ever can. That is not a
       redundancy: it is why step 4's fragment arm is unreachable and its DOCTYPE arm is not. */
    "try { document.body.moveBefore(new DocumentFragment(), null); mbThrew += ':none'; }"
      "catch (e) { mbThrew += ':' + e.name; }"
    /* STEP 3's NotFoundError, over a reference child of a different parent. */
    "try { mbA.moveBefore(mbB, document.body.firstChild); mbThrew += ':none'; }"
      "catch (e) { mbThrew += ':' + e.name; }"
    /* STEP 4 IS ONLY REACHABLE FOR A DOCTYPE, and only for one already in this tree — `document.doctype` is
       the only such node there is. Its presence is reported beside the throw rather than assumed, because a
       null one would make this a TypeError from the IDL and read as a pass by name. */
    "var mbDt = document.doctype;"
    "try { document.body.moveBefore(mbDt, null); mbThrew += ':none'; } catch (e) { mbThrew += ':' + e.name; }"
    "fetch('/api/movebeforethrow?v=' + encodeURIComponent(mbThrew)"
    " + '&doctype=' + (mbDt ? 'has' : 'null'));"
    /* THE STATE-PRESERVING HALF. The element is appended (one `c`), then moved between two connected parents
       (one `m` and nothing else). A move written as remove-then-insert reads 'cdc' here and the tree looks
       identical either way, which is the whole reason this is the assertion. */
    "var mbP1 = document.createElement('div'); var mbP2 = document.createElement('div');"
    "document.body.append(mbP1, mbP2);"
    "var mbEl = document.createElement('x-moved'); mbP1.appendChild(mbEl);"
    "mbP2.moveBefore(mbEl, null);"
    "fetch('/api/movebeforece?v=' + (mbCe.join('') === 'cm' && mbEl.parentNode === mbP2"
    " ? 'ispreserved' : mbCe.join('') || 'none'));"
    /* §13.4 A PARSE OF THE PAGE'S SIZE. The tokeniser is fed ONE BYTE per step, so this assignment suspends
       about two thousand times — and a resume that loses the tokeniser's position gives a DIFFERENT TREE, not
       a slower one, which is what the counts below catch. The markup is built by doubling rather than by a
       60-iteration loop because the loop is what forced execution explores, not the parse. */
    "var bigOne = '<li class=\"r\"><b>c</b>t</li>';"
    "for (var bpI = 0; bpI < 4; bpI++) bigOne += bigOne;"
    "var bigHost = document.createElement('ul'); document.body.appendChild(bigHost);"
    "bigHost.innerHTML = bigOne + '<li class=\"last\"><b>z</b></li>';"
    "fetch('/api/bigparse?n=' + bigHost.children.length + '&b=' + bigHost.querySelectorAll('b').length"
    " + '&last=' + bigHost.lastElementChild.getAttribute('class')"
    " + '&len=' + bigOne.length);"
    /* IDENTITY AT SCALE. One JS object per Lexbor node is what makes `n === n` true, and the map behind it is
       keyed by the node's address — so a hash that mishandles a collision hands out a SECOND object for a node
       and every comparison against the first is silently false. A page hits this constantly (a Set of visited
       nodes, a WeakMap keyed by node, `el.parentNode === container`), and nothing throws when it breaks.
       Two independent walks over the same 34 nodes, compared both ways. */
    "var idSeen = new Set(), idAll = [], idOk = 1;"
    "var idW = function(el) { idSeen.add(el); idAll.push(el);"
      "for (var c = el.firstElementChild; c; c = c.nextElementSibling) idW(c); };"
    "for (var idI = 0; idI < bigHost.children.length; idI++) idW(bigHost.children[idI]);"
    "for (var idJ = 0; idJ < idAll.length; idJ++) if (!idSeen.has(idAll[idJ])) idOk = 0;"
    "var idB = bigHost.querySelectorAll('b');"
    "for (var idK = 0; idK < idB.length; idK++) if (!idSeen.has(idB[idK])) idOk = 0;"
    "fetch('/api/nodeident?n=' + idSeen.size + '&all=' + idAll.length"
    " + '&ok=' + (idOk && bigHost.firstElementChild === bigHost.children[0] ? 'same' : 'split'));"
    /* §4.2.3 THE INSERTION STEPS, now drained by a machine instead of walked inside the chokepoint.
       (1) ORDERING IS UNCHANGED, which is the whole constraint: the steps run synchronously as part of the
       insertion, so an element is UPGRADED by the time appendChild returns and a method its constructor
       installed is callable on the very next statement. A deferred job would fail this.
       (2) THE WALK IS OF THE PAGE'S SIZE: 120 custom elements nested 120 deep, inserted by ONE appendChild, is
       one call whose steps visit every one of them — and a resume that restarts or skips gives a wrong count. */
    "var upN = 0, upSync = 'notyet';"
    /* The count is reported by the LAST callback rather than read after the inserts: a connected reaction is
       ENQUEUED, so nothing has run yet when appendChild returns. If the walk skips or repeats a node the count
       never reaches 121 and this endpoint is simply never fetched, which is what the oracle checks. */
    "customElements.define('up-x', class extends HTMLElement {"
      "connectedCallback() { upN++; if (upN === 121) fetch('/api/insertsteps?sync=' + upSync + '&deep=' + upN); }"
      "mark() { return 'up'; } });"
    "var upHost = document.createElement('div'); document.body.appendChild(upHost);"
    "var upOne = document.createElement('up-x'); upHost.appendChild(upOne);"
    "upSync = (typeof upOne.mark === 'function') ? upOne.mark() : 'notyet';"
    "var upDeep = document.createElement('div'), upCur = upDeep;"
    "for (var upI = 0; upI < 120; upI++) {"
      "var upE = document.createElement('up-x'); upCur.appendChild(upE); upCur = upE; }"
    "document.body.appendChild(upDeep);"
    /* §4.7 DocumentFragment. It was ABSENT in the worst shape a gap takes: a fragment node HAD a wrapper and
       answered nodeType 11, while every member that makes one useful was undefined — nothing threw, the page
       took the branch behind the undefined, and the engine reported that branch's surface. §4.12.3's
       `template.content` was the same: `t.content.querySelector(...)`, the ordinary way to use a template,
       threw about a property of undefined instead of naming the missing member.
       The mixin is why this is short: DocumentFragment includes ParentNode, so consolidating that first is what
       makes these members exist rather than a third copy of them. */
    "var df = new DocumentFragment();"
    "var dfa = document.createElement('p'); dfa.setAttribute('id', 'dfid');"
    "df.appendChild(dfa); df.appendChild(document.createTextNode('t'));"
    "df.append(document.createElement('b'));"
    "var dupHost = document.createElement('div'); document.body.appendChild(dupHost);"
    "dupHost.innerHTML = '<i id=\"dup\">1</i><u id=\"dup\">2</u>';"
    "var tq = document.createElement('div');"
    "tq.innerHTML = '<template><p class=\"tc\">in</p><i></i></template>';"
    "var tqe = tq.firstChild;"
    "fetch('/api/fragment?i=' + (df instanceof DocumentFragment && df instanceof Node"
    " && df.nodeType === 11 ? 'iface' : 'wrong')"
    " + '&pn=' + (df.children.length === 2 && df.childNodes.length === 3"
    " && df.querySelector('p') === dfa && df.querySelectorAll('p').length === 1"
    " && df.getElementById('dfid') === dfa && df.getElementById('nope') === null ? 'mixin' : 'wrong')"
    /* §4.2.4 is ONE member over its RECEIVER. Document's copy searched a global instead, so a scope that is
       not the document had no way to be wrong — and it took the FIRST in tree order by collecting every match
       and then indexing 0, which is a different algorithm wearing the same name. Both are asserted: the
       fragment's id is invisible to the document, and a DUPLICATE id answers with the first. */
    " + '&scope=' + (document.getElementById('dfid') === null ? 'receiver' : 'global')"
    /* [SameObject]: the fragment's wrapper is ONE object, so a page stashing t.content still has it. */
    " + '&first=' + (document.getElementById('dup').textContent === '1' ? 'tree-order' : 'wrong')"
    " + '&tc=' + (tqe.content === tqe.content && tqe.content instanceof DocumentFragment"
    " && tqe.content.querySelector('.tc').textContent === 'in'"
    " && tqe.content.childElementCount === 2 && tqe.childNodes.length === 0 ? 'content' : 'wrong'));"
    /* §4.5/§4.9 getElementsByTagName / getElementsByClassName — LIVE and over the RECEIVER, both of which the
       old Document-only copy got wrong: it searched a global root and answered with a static Array, which its
       own comment named as a gap, and Element did not have them at all. Liveness is the half a static snapshot
       cannot express: read the length, insert a matching element, read it again. */
    "var gbn = document.createElement('div'); document.body.appendChild(gbn);"
    "gbn.innerHTML = '<i class=\"a b\">1</i><p><i class=\"a\">2</i></p><i class=\"b\">3</i>';"
    "var gbnI = gbn.getElementsByTagName('i'), gbnA = gbn.getElementsByClassName('a');"
    "var gbnAB = gbn.getElementsByClassName('b a'), gbnStar = gbn.getElementsByTagName('*');"
    "var gbn0 = gbnI.length + ':' + gbnA.length + ':' + gbnAB.length + ':' + gbnStar.length;"
    "var gbnNew = document.createElement('i'); gbnNew.setAttribute('class', 'a b'); gbn.appendChild(gbnNew);"
    "var gbn1 = gbnI.length + ':' + gbnA.length + ':' + gbnAB.length + ':' + gbnStar.length;"
    "fetch('/api/byname?before=' + gbn0 + '&after=' + gbn1"
    /* descendant, not children: the <i> inside the <p> counts, and the <p> itself does not match 'i' */
    /* descendant, not children: the <i> inside the <p> counts, and the <p> itself does not match 'i' */
    " + '&deep=' + (gbnI[1].textContent === '2' && gbnI[1].parentNode.tagName === 'P' ? 'descend' : 'wrong')"
    /* §4.5 matches the query case-insensitively in an HTML document — `DIV` is how a lot of older code spells it */
    " + '&ci=' + (gbn.getElementsByTagName('I').length === gbnI.length ? 'nocase' : 'wrong')"
    /* §4.9 tagName is the HTML-UPPERCASED qualified name, and must agree with nodeName, which already was */
    " + '&tag=' + (gbnNew.tagName === 'I' && gbnNew.tagName === gbnNew.nodeName"
    " && gbnNew.localName === 'i' ? 'upper' : 'wrong')"
    /* the RECEIVER scopes it: an <i> outside `gt` is not in `gt`'s collection but is in the document's */
    " + '&scope=' + (document.getElementsByTagName('i').length > gbnI.length ? 'receiver' : 'global')"
    " + '&live=' + (gbnI instanceof HTMLCollection && typeof gbnI.map === 'undefined' ? 'iface' : 'wrong'));"
    /* THE SOURCE'S BROWSER TRANSFORM decides whether a breakout is real, and the SAME candidate through the
       SAME source answers differently per sink context. A fragment percent-encodes `<` but NOT the apostrophe,
       so:
         - a JS-context sink fed from location.hash IS a real XSS (`';X9()//` arrives intact), and
         - an HTML-context sink fed from the same source is NOT (the `<` arrives as %3C and parses as text).
       Before the delivery transform existed the solver handed the payload over raw and would report BOTH as
       working, which is a false PoC — the thing this half of the engine must never produce.
       BOTH HALVES ARE ASSERTED, through ONE source into TWO sink contexts, which is the only arrangement that
       can tell "the transform is applied" apart from "the solver is failing to solve": the apostrophe survives
       the fragment set so the JS sink FIRES (through `'#';X9()//'` — the leading `#` included), while `<` does
       not survive so the HTML sink CANNOT, and it is reported as a PARKED SEARCH rather than omitted. The
       negative half costs the run TWO extra candidate re-fires that are known not to fire — the markup sink's
       context PROBE and the one escape solve_html.c derives from it — where the deleted CANDS_HTML spray cost
       five; that cost buys the one property the @S half must never lose, so it is paid on every build. */
    "eval(\"'\" + location.hash + \"'\");"
    "var lhHost = document.createElement('div'); document.body.appendChild(lhHost);"
    "lhHost.innerHTML = location.hash;"
    /* AND THE SAME SOURCE INTO AN ATTRIBUTE-VALUE HOLE, which is the statement that §@S's three observations
       are solved JOINTLY rather than one at a time. The write above puts the attacker's bytes in §13.2.5.1's
       data state, whose ONLY exit is `<` — the fragment percent-encode set (URL §1.3 "Percent-encoded bytes")
       holds it, so that search is unsolvable through this source and the entry above says so. This one puts
       them in §13.2.5.37 "Attribute value (single-quoted) state", which is left by the apostrophe the fragment
       set does NOT hold, into §13.2.5.39 "After attribute value (quoted) state", which is left by whitespace
       AND by U+002F SOLIDUS — and no percent-encode set holds the solidus. So an escape EXISTS here that needs
       no `<`, no `>` and no space, and a derivation that reads only the sink's parse context constructs the
       one spelling the source cannot carry, reports it as an escape that merely did not fire, and misses a
       real raw-fragment XSS. The two writes together are the only arrangement that tells "the source transform
       is applied" apart from "the derivation stopped at the first spelling": the first must NOT fire and the
       second MUST.
       IT IS A DERIVED SOURCE IDENTITY ON PURPOSE. `location.hash.slice(1)` composes `{location.hash}.slice()`
       (concolic.h states that derivation byte for byte), so this is its OWN search with its own probes — the
       negative half above keeps its parked entry instead of being absorbed into a search that fires. The ROOT
       is still `location.hash`, which is what carries the percent-encode set through the derivation, so the
       constraint this search is solved under is the same one. */
    "var lhAttr = document.createElement('div'); document.body.appendChild(lhAttr);"
    "lhAttr.innerHTML = \"<img alt='\" + location.hash.slice(1) + \"'>\";"
    /* THE OUTCOME FORK AT A C BUILTIN. `JSON.parse` of unknown text has two feasible completions — 25.5.1
       step 8's value and step 2's SyntaxError — and a builtin that picks one has DELETED the arm the `catch`
       and everything behind it lives on. BOTH endpoints below must appear in ONE run: two flows, one
       snapshot-forked from the other AT the builtin.
       AND THE REFINEMENT, which is the negative half and the reason this is two statements and not one: the
       SAME builtin over `location.hash` ITSELF has only ONE feasible completion, because the component that
       owns that source declares what the browser delivers — the empty string or `#` followed by the fragment,
       neither of which is a JSON text. So that one must throw with NO fork, exactly as V8 does, and
       /api/jsonrawok must never be reached. Asserting only the fork would pass with the refinement absent;
       asserting only the throw would pass with the fork absent. */
    "try { JSON.parse(location.hash.slice(1)); fetch('/api/jsonok'); }"
    "catch (jpe) { fetch('/api/jsonthrew?n=' + jpe.name); }"
    "try { JSON.parse(location.hash); fetch('/api/jsonrawok'); }"
    "catch (jpe2) { fetch('/api/jsonrawthrew?n=' + jpe2.name); }"
    "fetch('/api/locsrc?o=' + location.origin + '&pn=' + location.pathname);"
    /* §3.1.5's element shortcuts and §4.5's createDocumentFragment. All five shortcuts are LIVE
       HTMLCollections over the document — a bundle scanner reaches for document.scripts and document.forms in
       particular, and with them absent the loop over them never ran and nothing said why.
       `links` is the one that is a PREDICATE rather than a tag: `a`/`area` WITH an href, so an anchor used as a
       scroll target is not a link. */
    "var scHost = document.createElement('div'); document.body.appendChild(scHost);"
    "scHost.innerHTML = '<form></form><img><a href=\"/l1\">l</a><a name=\"nolink\">n</a>'"
      "+ '<area href=\"/l2\"><embed>';"
    "var scF = document.forms, scL = document.links;"
    "var scBefore = scF.length + ':' + document.images.length + ':' + scL.length"
      "+ ':' + document.embeds.length;"
    "var scMore = document.createElement('form'); scHost.appendChild(scMore);"
    "var scFrag = document.createDocumentFragment();"
    "scFrag.appendChild(document.createElement('b')); scFrag.appendChild(document.createElement('i'));"
    "var scFragN = scFrag.childNodes.length; scHost.appendChild(scFrag);"
    "fetch('/api/docshort?before=' + scBefore + '&after=' + scF.length"
    /* §4.2.3: appending a fragment inserts its CHILDREN, so the last child is the <i> element and not the
       fragment itself — nodeType 1, which was 11 when the fragment node went into the tree instead. */
    " + '&fragnode=' + scHost.lastChild.nodeType"
    /* the anchor with no href is NOT a link, and the <area> with one IS */
    " + '&links=' + scL[0].getAttribute('href') + ',' + scL[1].getAttribute('href')"
    " + '&iface=' + (scF instanceof HTMLCollection && scFrag instanceof DocumentFragment ? 'live' : 'wrong')"
    /* §4.2.3: appending a fragment moves its CHILDREN, leaving the fragment empty */
    " + '&frag=' + scFragN + ':' + scFrag.childNodes.length"
    " + ':' + scHost.getElementsByTagName('b').length);"
    /* §4.9/§4.9.1/§4.9.2 attributes, NamedNodeMap and Attr. `el.attributes` was absent and so was every object
       behind it, which is a gap with a particular shape: the loops a page writes over an element's attributes
       — copying them onto a clone, deciding which to forward — are `for (const a of el.attributes)`, and with
       no attributes there is no iteration and no error either. The loop body simply never ran.
       An Attr IS a node, so it gets identity from the one wrapper table for free; what was missing was a
       PROTOTYPE for node type 2, so an attribute answered nodeType 2 with `name` and `value` undefined. */
    "var atE = document.createElement('div'); document.body.appendChild(atE);"
    "atE.setAttribute('id', 'atid'); atE.setAttribute('data-k', 'v');"
    "var atM = atE.attributes, atSeen = [];"
    "for (var atI = 0; atI < atM.length; atI++) atSeen.push(atM[atI].name + '=' + atM[atI].value);"
    "var atIter = []; for (const a of atE.attributes) atIter.push(a.name);"
    "atM.getNamedItem('data-k').value = 'w';"
    "var atRm = 'none';"
    "try { atM.removeNamedItem('nope'); } catch (e) { atRm = e.name; }"
    "atM.removeNamedItem('id');"
    "fetch('/api/attrs?list=' + atSeen.join(',')"
    " + '&iter=' + atIter.join(',')"
    /* the value setter is setAttribute's change steps, so the element really changed */
    " + '&set=' + atE.getAttribute('data-k')"
    /* §4.9.1: a name that is not there is a NotFoundError, not a quiet no-op */
    " + '&miss=' + atRm + '&removed=' + (atE.getAttribute('id') === null ? 'gone' : 'stayed')"
    /* an Attr is a Node with identity, and [SameObject] holds for the map */
    " + '&iface=' + (atE.attributes === atM && atM instanceof NamedNodeMap"
    " && atM[0] instanceof Attr && atM[0] instanceof Node && atM[0].nodeType === 2"
    " && atM[0] === atM[0] && atM[0].ownerElement === atE ? 'node' : 'wrong')"
    " + '&names=' + atE.getAttributeNames().join(','));"
    /* §3.2.2 dataset. It was ABSENT, so `el.dataset.userId` read undefined — and undefined does not throw, so a
       page storing its routing on data-* took the branch behind it and the engine reported THAT branch's
       surface. The mapping is the spec and it is asymmetric on purpose: `data-user-id` is `userId`, an
       property name with a dash before a lowercase letter is a SyntaxError rather than a new attribute.
       `data-X` is NOT a counter-example to the uppercase rule: §4.9's setAttribute lowercases the qualified
       name in an HTML document, so it is stored as `data-x` and is exposed as `x`, which is what a browser does
       too. The rule about an uppercase surviving `data-` is about a name that could only arrive through a
       namespace-aware setter, and it is implemented for when one lands. */
    "var dsE = document.createElement('div'); document.body.appendChild(dsE);"
    "dsE.setAttribute('data-user-id', 'u7');"
    "dsE.setAttribute('data-X', 'hidden');"
    "dsE.setAttribute('title', 'notdata');"
    "dsE.dataset.roleName = 'admin';"
    "var dsBad = 'none';"
    "try { dsE.dataset['a-b'] = 'x'; } catch (e) { dsBad = e.name; }"
    "var dsKeys = Object.keys(dsE.dataset).sort().join(',');"
    "fetch('/api/dataset?read=' + dsE.dataset.userId"
    /* the write went through setAttribute's chokepoint, so it is a real attribute with the mangled name */
    " + '&wrote=' + dsE.getAttribute('data-role-name')"
    " + '&keys=' + dsKeys"
    /* an uppercase after data- is not a supported property name, and a non-data attribute is not one either */
    /* the case §4.9 lowercased, and an attribute that is not data-* at all */
    " + '&skip=' + (dsE.dataset.X === undefined && dsE.dataset.x === 'hidden'"
    " && dsE.dataset.title === undefined ? 'unexposed' : 'leaked')"
    " + '&bad=' + dsBad"
    " + '&same=' + (dsE.dataset === dsE.dataset ? 'sameobject' : 'fresh')"
    " + '&del=' + ((delete dsE.dataset.userId), dsE.getAttribute('data-user-id') === null ? 'gone' : 'stayed'));"
    /* PROBE §4.2.6 scoped matching: `el.querySelectorAll('div p')` must match a <p> inside el whose <div>
       ancestor is OUTSIDE el — the selector is evaluated against the whole document and only the RESULTS are
       filtered to el's subtree. An implementation that walks el's subtree in isolation misses it. */
    "var scOuter = document.createElement('div'); document.body.appendChild(scOuter);"
    "var scMid = document.createElement('section'); scOuter.appendChild(scMid);"
    "scMid.innerHTML = '<p id=\"scp\">x</p>';"
    "var qsThrew = 'none';"
    "try { scMid.querySelector('###'); } catch (e) { qsThrew = e.name; }"
    "var qaThrew = 'none';"
    "try { document.querySelectorAll(':::'); } catch (e) { qaThrew = e.name; }"
    "fetch('/api/scopesel?anc=' + scMid.querySelectorAll('div p').length"
    " + '&self=' + scMid.querySelectorAll('section p').length"
    " + '&plain=' + scMid.querySelectorAll('p').length"
    /* §4.2.6: an unparseable selector is a SyntaxError from ALL FOUR members. matches and closest already
       threw; the two queries answered null and an empty list, so a page with a typo in a selector was told
       "no such element" and ran the branch behind that answer. */
    " + '&bad=' + qsThrew + ':' + qaThrew"
    /* closest walks INCLUSIVE ancestors and compiles the selector ONCE now, not once per level */
    " + '&closest=' + (scMid.querySelector('p').closest('div') === scOuter"
    " && scMid.querySelector('p').closest('p').tagName === 'P' ? 'up' : 'wrong'));"
    /* §4.4 textContent's READ is a walk of the SUBTREE, so it is a machine now. What it must reproduce exactly
       is WHICH nodes count: Text nodes only, over child links — so a comment contributes nothing, and a
       `<template>`'s content is not part of its element's text because it is not under it. */
    "var tcx = document.createElement('div');"
    "tcx.innerHTML = 'a<b>B</b><!--no--><template>HID</template>c';"
    "fetch('/api/textwalk?v=' + tcx.textContent"
    " + '&deep=' + gbn.textContent"
    " + '&big=' + bigHost.textContent.length);"
    /* §4.2.11's NAMED getter: `children.foo` is how a great deal of older code reaches its own markup. */
    "var nb = document.createElement('u'); nb.setAttribute('id', 'namedkid'); lv.appendChild(nb);"
    "fetch('/api/named?v=' + (lv.children.namedItem('namedkid') === nb && lv.children.namedkid === nb"
    " && lv.children.nosuch === undefined ? 'isnamed' : 'wrong'));"
    /* §4.9 matches / closest, and §7.1's DOMTokenList — the two questions a router asks and the way a bundle
       gates a branch of its UI. classList holds NO tokens of its own: they are the `class` attribute split, so
       a write through the list and a write through setAttribute are the same write, and it time-travels
       because the attribute does. `classList === classList` is [SameObject] and a page relies on it. */
    "var cl = document.createElement('div'); cl.className = 'a b'; document.body.appendChild(cl);"
    "var cll = cl.classList;"
    "cll.add('c'); cll.add('a');"          /* already present: a set, so no duplicate */
    "cll.remove('b');"
    "var tg1 = cll.toggle('d'); var tg2 = cll.toggle('d');"        /* on, then off */
    "var tg3 = cll.toggle('a', true);"     /* force keeps it, and answers force */
    "var rp = cll.replace('c', 'e');"
    /* 'a b' +c +a(dup) -b +d -d a(force) c->e  ==>  'a e' */
    "fetch('/api/classlist?v=' + (cl.getAttribute('class') === 'a e'"
    " && tg1 === true && tg2 === false && tg3 === true && rp === true"
    " && cll.contains('e') && !cll.contains('b') && cll.length === 2 && cll.item(0) === 'a'"
    " && cl.classList === cll ? 'iscl' : 'wrong'));"
    /* §3.9's INDEXED PROPERTY GETTER — `list[0]` is a LOOKUP, not a property, which is the whole difference:
       it answers against the attribute as it is NOW, so it cannot go stale the way a written-out index would.
       And §3.7.10 gives an interface with one %Array.prototype.values% as its @@iterator, so `for..of` and
       spread over a classList are ordinary code that had nothing. */
    "var clj = [];"
    "for (var cq = 0; cq < cll.length; cq++) clj.push(cll[cq]);"
    "var clspread = [].concat.apply([], [[...cll]]);"
    "var clfor = ''; for (var ct of cll) clfor += ct;"
    "var clkeys = Object.keys(cll).join(',');"
    "cll.add('z');"   /* the lookup is live: index 2 exists only after this */
    "fetch('/api/clindex?v=' + (clj.join('') === 'ae' && clspread.join('') === 'ae' && clfor === 'ae'"
    " && clkeys === '0,1' && cll[2] === 'z' && cll[9] === undefined && !(5 in cll) && (1 in cll)"
    " ? 'isindex' : 'wrong'));"
    "cll.remove('z');"
    /* The list is a VIEW: a write through the attribute is visible through the list, with nothing to sync. */
    "cl.setAttribute('class', 'x y'); "
    "fetch('/api/clview?v=' + (cll.length === 2 && cll.contains('y') && String(cll) === 'x y'"
    " ? 'isview' : 'wrong'));"
    "var m1 = cl.matches('div.x'); var m2 = cl.matches('span');"
    "var inner = document.createElement('b'); cl.appendChild(inner);"
    "var c1 = inner.closest('div'); var c2 = inner.closest('nav');"
    "fetch('/api/matches?v=' + (m1 && !m2 && c1 === cl && c2 === null"
    " && inner.closest('b') === inner ? 'ismatch' : 'wrong'));"
    /* §4.9: a selector that does not parse is a SyntaxError, never a quiet `false`. */
    "var msy = 'wrong'; try { cl.matches('!!!'); } catch (e) { msy = 'issyn'; }"
    "fetch('/api/matchbad?v=' + msy);"
    /* §2.9 THE PROPAGATION PATH. One dispatch walks the target and then its ancestors: `target` stays put,
       `currentTarget` moves, and the phase goes AT_TARGET then BUBBLING_PHASE. The old delivery enqueued each
       listener as its own job with no walk between them, so none of this could be observed. */
    "var bpar = document.createElement('section'); var bkid = document.createElement('em');"
    "bpar.appendChild(bkid); document.body.appendChild(bpar); var btrace = '';"
    "bkid.addEventListener('bub', function(e){ btrace += 'K' + e.eventPhase + (e.target === bkid ? 't' : '?')"
    " + (e.currentTarget === bkid ? 'c' : '?'); });"
    "bpar.addEventListener('bub', function(e){ btrace += 'P' + e.eventPhase + (e.target === bkid ? 't' : '?')"
    " + (e.currentTarget === bpar ? 'c' : '?'); });"
    "bkid.dispatchEvent(new Event('bub', { bubbles: true }));"
    "fetch('/api/bubble?v=' + (btrace === 'K2tcP3tc' ? 'isbubble' : 'wrong'));"
    /* An event that does not bubble reaches the target alone, and stopPropagation ends the walk after it. */
    "btrace = ''; bkid.dispatchEvent(new Event('bub'));"
    "var nb = btrace;"
    "btrace = ''; bkid.addEventListener('stp', function(e){ btrace += 'K'; e.stopPropagation(); });"
    "bpar.addEventListener('stp', function(e){ btrace += 'P'; });"
    "bkid.dispatchEvent(new Event('stp', { bubbles: true }));"
    "fetch('/api/nobubble?v=' + (nb === 'K2tc' && btrace === 'K' ? 'isnobub' : 'wrong'));"
    /* §2.9's CAPTURING leg. A capturing listener on the ancestor runs BEFORE the target's, at eventPhase 1 —
       the walk goes down the path and then back up it, which is why the target is its own leg and not an end
       of one. A registration with no options is not capturing, so the two must interleave in that order. */
    "btrace = '';"
    "bpar.addEventListener('cap', function(e){ btrace += 'Pc' + e.eventPhase; }, true);"
    "bpar.addEventListener('cap', function(e){ btrace += 'Pb' + e.eventPhase; });"
    "bkid.addEventListener('cap', function(e){ btrace += 'K' + e.eventPhase; });"
    "bkid.dispatchEvent(new Event('cap', { bubbles: true }));"
    "fetch('/api/capture?v=' + (btrace === 'Pc1K2Pb3' ? 'iscap' : 'wrong'));"
    /* …AND THE SAME LEG DECIDED OUT OF UNKNOWN EXTERNAL INPUT, which is the options bag a real bundle passes:
       `el.addEventListener(t, f, cfg.listen)` where `cfg` came from somewhere this engine cannot see. Web IDL
       §3.2.25 Union types picks the arm and DOM §2.7 Interface EventTarget's flatten options step 2 then reads
       `capture` off it — and on either arm that value is another unknown, so ONE registration is TWO: a
       capturing listener and a bubbling one, which the dispatch tells apart by the phase it runs at. A
       ToBoolean at the boundary answered `true` for every unknown there has ever been (a concolic wears an
       Object and every Object is truthy), so only the capturing world existed and the bubbling one — the
       DEFAULT the IDL writes, `boolean capture = false` — was the arm that never ran. Both phases must appear;
       one of them alone is the collapse. */
    "bpar.addEventListener('unkcap', function(e){ fetch('/api/aelunk?v='"
    " + (e.eventPhase === 1 ? 'aelcapturing' : 'aelbubbling')); }, state.listen);"
    "bkid.dispatchEvent(new Event('unkcap', { bubbles: true }));"
    /* §2.7's dedup key is (type, callback, CAPTURE), so ONE function registered both ways is TWO listeners —
       and removing it with the wrong flag removes neither. */
    "var dupn = 0; var dupf = function(){ dupn++; };"
    "bpar.addEventListener('dup', dupf, true); bpar.addEventListener('dup', dupf);"
    "bpar.addEventListener('dup', dupf, true);"   /* the exact same registration: does nothing */
    "bpar.dispatchEvent(new Event('dup'));"
    "var dup1 = dupn; bpar.removeEventListener('dup', dupf); dupn = 0;"
    "bpar.dispatchEvent(new Event('dup'));"
    "fetch('/api/dedup?v=' + (dup1 === 2 && dupn === 1 ? 'isdedup' : 'wrong'));"
    /* §2.9 "inner invoke" step 2: `once` removes the listener BEFORE calling it, so a re-entrant dispatch from
       inside the handler cannot run it a second time — which is exactly what `once` exists to prevent, and it
       is also the only way to tell "removed before" from "removed after". */
    "var oncen = 0;"
    "bpar.addEventListener('one', function(){ oncen++; if (oncen < 5) bpar.dispatchEvent(new Event('one')); },"
    " { once: true });"
    "bpar.dispatchEvent(new Event('one'));"
    "bpar.dispatchEvent(new Event('one'));"
    "fetch('/api/once?v=' + (oncen === 1 ? 'isonce1' : 'wrong'));"
    /* THE ENGINE'S OWN FIRE goes through the SAME walk. DOMContentLoaded is fired AT THE DOCUMENT and bubbles
       to the window — which used to need the window passed in by hand as a second fire_at, and is now just the
       document's ancestor. A trusted event, which is what tells it from one the page dispatched. */
    "addEventListener('DOMContentLoaded', function(e){"
    " fetch('/api/dclbubble?v=' + (e.target === document && e.currentTarget === globalThis && e.isTrusted"
    " && e.eventPhase === 3 ? 'isdcl' : 'wrong')); });"
    /* §3.2 abort() is SYNCHRONOUS: the listener has run by the time abort() returns, so a flag it set is
       already readable on the next line. A queued fire answers that question after the caller returned, which
       is what this used to do. The listener holds a LOOP, so the dispatch suspends inside it and abort()
       resumes at the listener it parked on — synchronous does not mean uninterruptible. */
    "var sc = new AbortController(); var sflag = 0;"
    "sc.signal.addEventListener('abort', function(){ var n = 0; for (var i = 0; i < 400; i++) { n += i; }"
    " sflag = n; });"
    "sc.abort();"
    "fetch('/api/abortsync?v=' + (sflag === 79800 && sc.signal.aborted ? 'issync' : 'wrong'));"
    /* §3.2: a second abort() fires nothing — the state half answers whether there is anything to fire. */
    "var sonce = sflag; sc.abort();"
    "fetch('/api/abortonce?v=' + (sflag === sonce ? 'isonce' : 'wrong'));"
    /* §4.10 FORMS — a submission is a REQUEST the page's own code composes, and submit() DERIVES it without
       sending anything, which is the rule for a state-mutating request. The value a field carries is the
       endpoint's example value, and when it is an ATTACKER SOURCE the finding says so rather than inventing
       one — which is why the value STATE holds a JSValue and not bytes. */
    "var fq = document.createElement('form'); fq.action = '/api/search';"
    "var fi = document.createElement('input'); fi.name = 'q'; fq.appendChild(fi); fi.value = state.q;"
    "var fd = document.createElement('input'); fd.name = 'off'; fd.setAttribute('disabled',''); fq.appendChild(fd);"
    "var fc = document.createElement('input'); fc.name = 'agree'; fc.setAttribute('type','checkbox');"
    "fq.appendChild(fc); document.body.appendChild(fq); fq.submit();"
    "fc.checked = true; fq.submit();"
    /* NOT `fi.value === state.q` — that compares two CONCOLICS, which forks both ways and reports both
       answers. The state's presence is proved by the submission carrying {state}.q; what this asserts is the
       tree-shaped half, which is concrete. */
    "fetch('/api/formstate?v=' + (fq.elements.length === 3 && document.forms.length >= 1 ? 'isform' : 'wrong'));"
    /* §4.10.21.4 requestSubmit() fires a CANCELABLE `submit` FIRST, and submits only if nothing cancelled it —
       a page that cancels is doing its own request instead, so recording the form's would be a finding the page
       never makes. Only an ABSENCE can prove that half. */
    "var fr = document.createElement('form'); fr.action = '/api/never'; document.body.appendChild(fr);"
    "fr.addEventListener('submit', function(e){ e.preventDefault(); }); fr.requestSubmit();"
    "var fo = document.createElement('form'); fo.action = '/api/didsubmit';"
    "var foi = document.createElement('input'); foi.name = 'ok'; fo.appendChild(foi);"
    "document.body.appendChild(fo); foi.value = 'rs'; fo.requestSubmit();"
    /* §4.13 CUSTOM ELEMENTS — the reason this component exists: connectedCallback's body is code NOTHING ELSE IN
       THE PROGRAM CALLS. The endpoints below are reachable only through the lifecycle, so their presence is the
       whole claim. `extends HTMLElement` is what a real bundle writes, and after the upgrade `this.setAttribute`
       must still resolve — that is the prototype CHAIN, not just the class's own prototype. */
    "class XPanel extends HTMLElement { connectedCallback(){ this.setAttribute('data-up','ceUP');"
    " fetch('/api/celife?v=' + (this.getAttribute('data-up') === 'ceUP' ? 'isce' : 'wrong')); } }"
    "customElements.define('x-panel', XPanel);"
    "document.body.appendChild(document.createElement('x-panel'));"
    /* §4.13.3 the RETROACTIVE upgrade: this element was in the tree as a plain HTMLElement before the name was
       defined. The expando set before the definition is still there afterwards, which is the identity half — the
       upgrade re-points the SAME wrapper, it does not build a new one. */
    "var ce0 = document.createElement('x-early'); ce0.marker = 'keptID'; document.body.appendChild(ce0);"
    "class XEarly extends HTMLElement { connectedCallback(){"
    " fetch('/api/ceearly?v=' + (this.marker === 'keptID' ? 'isearly' : 'wrong')); } }"
    "customElements.define('x-early', XEarly);"
    /* A reaction is a FLOW, not a C call: this one holds a loop AND an await, so it must park at both and resume.
       s = 0+..+299 = 44850 proves the loop ran to its end across the preempts. */
    "class XAsync extends HTMLElement { async connectedCallback(){ var n = 0; for (var i = 0; i < 300; i++) n += i;"
    " var w = await Promise.resolve('ceAWAIT'); fetch('/api/ceasync?n=' + n + '&w=' + w); } }"
    "customElements.define('x-async', XAsync);"
    "document.body.appendChild(document.createElement('x-async'));"
    /* §4.13.3 THE SUBTREE, and the removal reaction. A page that builds its UI off-tree and appends the ROOT
       once is the ordinary case, and the insertion steps used to run for the appended node alone — so a custom
       element inside a built fragment was never upgraded and its lifecycle code never ran. insertBefore did not
       run them at all. Both are the same one hook now, at the mutation chokepoint. */
    "class XDeep extends HTMLElement { connectedCallback(){ fetch('/api/cedeep?v=isdeep'); }"
    " disconnectedCallback(){ fetch('/api/cegone?v=isgone'); } }"
    "customElements.define('x-deep', XDeep);"
    "var dwrap = document.createElement('div'); var dinner = document.createElement('span');"
    "var ddeep = document.createElement('x-deep');"
    "dwrap.appendChild(dinner); dinner.appendChild(ddeep);"   /* all OFF-TREE: nothing has entered a document */
    "document.body.appendChild(dwrap);"                       /* one append, and the element two levels down runs */
    "document.body.removeChild(dwrap);"
    /* insertBefore is the OTHER tree write, and it never ran the steps at all. */
    "var dref = document.createElement('i'); document.body.appendChild(dref);"
    "document.body.insertBefore(document.createElement('x-deep'), dref);"
    /* §4.13.3 attributeChangedCallback. It runs only for a name the class declared as OBSERVED, which is why
       observedAttributes is read at DEFINE time — and reading it is the page's code (a static getter whose
       entries are themselves coerced), so the read and every entry's ToString are requests the declaration's
       step body parks on. The unwatched attribute proves the filter: only an ABSENCE can. */
    "class XAttr extends HTMLElement {"
    " static get observedAttributes(){ var n = 0; for (var i = 0; i < 30; i++) n += i;"
    "  return [{ toString: function(){ return n === 435 ? 'data-w' : 'bad'; } }]; }"
    " attributeChangedCallback(nm, ov, nv){ fetch('/api/ceattr?n=' + nm + '&o=' + ov + '&v=' + nv); } }"
    "customElements.define('x-attr', XAttr);"
    "var xa = document.createElement('x-attr'); document.body.appendChild(xa);"
    "xa.setAttribute('data-w', 'first');"
    "xa.setAttribute('data-w', 'second');"
    "xa.setAttribute('data-ignored', 'nope');"   /* not observed: no reaction, and only an absence proves it */
    "xa.removeAttribute('data-w');"
    /* §4.13.4 get(), and §4.13.1's name rule — a name with no hyphen is a SyntaxError, not a quiet registration. */
    "var cename = 'wrong'; try { customElements.define('nohyphen', XPanel); } catch (e) { cename = 'issyntax'; }"
    "fetch('/api/cename?v=' + cename);"
    /* …and the same throw UNCAUGHT, at a chunk's top level, where a page error is what it becomes. */
    "loadScript('/chunk/cethrow.js');"
    /* HTML §8.1.7.5 — the two claims that make the two lists necessary, and neither can be proved by the other.
       (1) A rejection nobody ever handles IS a page error: an async bundle delivers most of its errors this way
       and every one of them used to be silent. (2) A handler attached in a LATER microtask is ordinary correct
       code, so that rejection must never be reported — which only an ABSENCE can prove. */
    /* §8.1.7.5 fires `unhandledrejection` at the global, CANCELABLE, before anything is reported. A page that
       ships its own error reporter cancels it — and that reporter is code with a fetch in it, which is the
       surface this engine exists to reach, so the event is worth more than the report. Cancelling must
       SUPPRESS the report, which only the absence of the reason from the output can prove. */
    "addEventListener('unhandledrejection', function(e){ if (e.reason === 'rejCANCEL') { e.preventDefault();"
    " fetch('/api/rejevent?v=' + (e.promise && e instanceof PromiseRejectionEvent ? 'isrej' : 'wrong')); } });"
    "Promise.reject('rejCANCEL');"
    /* The interface a page can construct, and its REQUIRED member: PromiseRejectionEventInit.promise has no
       default, so an init without it is a TypeError from the declaration rather than a check in a body. */
    "var prereq = 'wrong'; try { new PromiseRejectionEvent('x', {}); } catch (e) { prereq = 'isreq'; }"
    "fetch('/api/prereq?v=' + prereq);"
    "var pe = new PromiseRejectionEvent('t', { promise: Promise.resolve(), reason: 'peREASON', cancelable: true });"
    "fetch('/api/prector?v=' + (pe.reason === 'peREASON' && pe instanceof Event && pe.cancelable && !pe.isTrusted"
    " ? 'isctor' : 'wrong'));"
    "Promise.reject('rejNOHANDLER');"
    "var prs = Promise.reject('rejSYNC'); prs.catch(function(){});"
    "var prh = Promise.reject('rejHANDLED');"
    "Promise.resolve().then(function(){ prh.catch(function(){}); });"
    "fetch('/api/ceget?v=' + (customElements.get('x-panel') === XPanel"
    " && customElements.get('x-none') === undefined ? 'isget' : 'wrong'));"
    /* A TYPED DICTIONARY MEMBER, converted through the page's own code. `extends` is a DOMString member of
       ElementDefinitionOptions, so Web IDL READS it (an accessor here, which a Proxy would make of any object)
       and then runs ToString on what it got — two suspension points inside one member, and the page's toString
       holds a loop, so the resume must come back to the CONVERSION and not re-run the read. The definition is
       then refused as a customized built-in, which is what proves the converted value arrived. */
    "var ceext = 'wrong';"
    "try { customElements.define('x-ext', class {}, { get extends(){ return { toString: function(){"
    " var n = 0; for (var i = 0; i < 120; i++) n += i; return n === 7140 ? 'button' : 'bad'; } }; } }); }"
    " catch (e) { ceext = 'isext'; }"
    "fetch('/api/ceext?v=' + ceext);"
    "if (cfg.admin) { setBodyAttr('data-tt','ttADMIN'); appendChild('kidADMIN'); rx.flag='flagADMIN'; fetch('/api/data?role=admin'); loadScript('/chunk/admin.js'); } else { setBodyAttr('data-tt','ttPUBLIC'); appendChild('kidPUBLIC'); rx.flag='flagPUBLIC'; fetch('/api/data?role=public'); }"   /* admin arm: same endpoint MERGES + a LAZY CHUNK loads. Each arm ALSO writes an attribute, appends a child node, AND assigns the ACCESSOR rx.flag (invokes the setter -> rx._f) -> per-flow DOM + heap-accessor writes across the EXISTING fork. */
    "fetch('/api/whoami?tt=' + getBodyAttr('data-tt'));"   /* DOM ATTR READ-BACK after the fork: per-flow -> admin flow reads ttADMIN, public flow reads ttPUBLIC */
    "fetch('/api/kid?mark=' + lastChildMark());"   /* DOM NODE READ-BACK: each flow's appended child is its OWN last child -> admin reads kidADMIN, public reads kidPUBLIC (neither's inserted node leaks) */
    "fetch('/api/flag?v=' + rx.flag);"   /* ACCESSOR READ-BACK: rx.flag reads rx._f (per-flow). WITHOUT the accessor-skip fix, cow_unapply would JS_SetProperty(rx.flag, undefined) -> re-invoke the setter -> corrupt _f -> v=undefined */
    "if (state.region === 'us-east-1') { fetch('/api/region/' + state.region); }"   /* EQ gate: true arm PINS region -> real @H value /api/region/us-east-1 */
    "eval(\"'\" + state.code + \"'\");"   /* @S JS: source lands INSIDE a §12.9.4 SingleStringCharacters state -> solve_js.c derives that state's own exit, `';X9()//` */
    /* @S JS, A SECOND §12 STATE, and the one that says the derivation is a derivation. The hole lands in a
       §12.4 SingleLineComment, which NONE of the five deleted CANDS_JS payloads could fit: every one of them
       continued the comment it was already inside, so this sink was unsolvable BY CONSTRUCTION and reported
       `parked, tried 5` without ever naming what it had failed to escape. solve_js.c scans the argument the
       probe run produced, names the state, and emits that state's own exit — §12.4 puts the LineTerminator
       OUTSIDE the comment and §12.10 rule 1 inserts the semicolon across it, so the whole escape is one
       newline and the call. */
    "eval('//' + state.note);"
    "setInnerHTML('<div>' + state.html + '</div>');"   /* @S HTML: source in HTML text -> breakout an auto-firing element */
    "setLocation(state.next);"   /* @S URL: attacker controls the whole URL -> breakout javascript:X9() */
    "Promise.resolve(cfg.admin ? 'thenADMIN' : 'thenPUBLIC').then(function(v){ fetch('/api/then?v=' + v); });"   /* ASYNC-AS-FLOW: a microtask reaction fires as part of THIS flow, under its COW -> admin flow reads thenADMIN, public thenPUBLIC (per-flow reaction isolation) */
    "(async function(){ var w = await Promise.resolve(cfg.admin ? 'awADMIN' : 'awPUBLIC'); fetch('/api/await?w=' + w); })();"   /* ASYNC FUNCTION + await: suspend at await, resume the continuation with the settled value under THIS flow's COW */
    "(async function(){ function h(f){ return f ? 'acADMIN' : 'acPUBLIC'; } fetch('/api/asynccall?w=' + h(cfg.admin)); })();"   /* ASYNC body calls a HELPER whose concolic branch forks DEEP (chain base->async->helper): the async frame is a CALLER, not the deepest — exercises clone_deep_flow's async-as-caller buffer sourcing (tramp_buf_base/tramp_live_sf) -> both acADMIN and acPUBLIC */
    "(async function(){ throw 'asyncThrew'; })().catch(function(e){ fetch('/api/caught?e=' + e); });"   /* async THROW -> rejected promise -> .catch reaction fires */
    "(async function(){ var s=0; for(var i=0;i<2000;i++){ s=s+1; } fetch('/api/asyncloop?s='+s); })();"   /* async body with a LOOP -> preempt may fire inside the async tramp frame */
    /* ORPHAN-INVOKE — the headline capability, and the ONE statement in this document that nothing in it calls.
       It asks two things at once because they are the two halves of the mechanism and either alone would pass
       while the other was broken: that the function RUNS at all (`/api/orphan/report`), and that its PARAMETER
       is unknown external input rather than `undefined` — so the equality gate FORKS and the arm behind it is
       explored (`/api/orphan/admin-only`), which is the whole of "learn the logged-in surface while logged
       out" reduced to one line. A driven orphan that received `undefined` would emit the first and never the
       second, and a run in which orphan-invoke does not exist emits neither: a page holding only this function
       measured ZERO endpoints against one flow, which is what this row exists to keep from happening again. */
    "function orphanNeverCalled(role){ fetch('/api/orphan/report');"
    " if (role === 'admin') { fetch('/api/orphan/admin-only'); } }"
    /* AND THE SEAM INSIDE ONE, which the two rows above cannot see: they assert an orphan RUNS and that its
       parameter is unknown, and both are true of a body driven start-to-finish with nothing to preempt at.
       A driven orphan's frame is a CALL-ROOT flow (JS_FlowNewCall), so a back-edge in its body must reach the
       preempt hook, park the flow as a COW snapshot and resume it — 3000 times here, under FORK_PREEMPT. The
       terminal value is in the PATH rather than a query parameter so one exact match decides it: `loop3000`
       says the loop ran to its end and every one of those resumes was byte-identical, where a dropped or
       reordered resume produces some other number and a lost one produces no endpoint at all. */
    "function orphanLoops(){ var s = 0; for (var i = 0; i < 3000; i++) { s = s + 1; }"
    " fetch('/api/orphan/loop' + s); }"
    /* AND AN ORPHAN THAT COMPARES ITS UNKNOWN AGAINST AN OBJECT, which is the FIRST thing a real library does
       with an argument it was handed and which took four unmodified bundles' engine instances down before this
       row existed: axios's `isPlainObject` and vue's own walk are both `while (n !== Object.prototype)`. The
       equality's other operand is an Object, and §7.1.19 ToString step 10 sends one to ToPrimitive — the page's
       valueOf/toString, run from a C activation with no flow base under it — so spelling it aborted the whole
       instance and every finding went with it. §7.2.14 IsStrictlyEqual step 1 returns false whenever
       SameType(x, y) is false, so there is nothing for such a comparison to pin the unknown to: the predicate
       forks and carries no pin. The claim is the endpoint BEHIND the walk, because an abort emits none. */
    "function orphanIdentity(o){ var n = o;"
    " while (n != null && n !== Object.prototype) { n = Object.getPrototypeOf(n); }"
    " fetch('/api/orphan/identity'); }"
    /* AND AN ORPHAN THAT PUTS ITS UNKNOWN THROUGH A COERCE-THEN-COMPUTE BUILTIN, which is the SECOND thing a
       real library does with an argument it was handed: every hand-rolled base64 / UTF-16 / hash decoder in a
       bundle is `String.fromCharCode(x.y)`. §22.1.2.1 step 2.a is `ℝ(? ToUint16(next))` and §7.1.11 step 1 is
       §7.1.4 ToNumber, while the declaration's own coercion is §7.1.1 ToPrimitive — which over an unknown is
       the identity — so the unknown reached the conversion boundary that owes C a real double and the whole
       instance aborted. The claim is the endpoint AFTER the call, because an abort emits none: reaching it says
       the coercion produced a value the string concatenation could carry rather than a crash. */
    "function orphanCharCode(e){ var s = String.fromCharCode(e.charCode);"
    " fetch('/api/orphan/charcode?c=' + s); }"
    /* AND THE POSTFIX UPDATE OPERATOR, WHICH IS HALF OF §13.4 AND WAS THE HALF NOBODY ASKED. `++i` reaches
       js_unary_arith_slow and its .arith hook answered; `i--` goes through §13.4.3.1 step 3's
       `? ToNumeric(? GetValue(lhs))` first and CONVERTED, so `for (; e--; )` — a real route loader's own loop —
       aborted the whole instance. Both spellings are here because the row must fail when only one is built:
       the PREFIX arm passed the entire time the postfix arm was crashing, so a fixture holding only `++i`
       reports a family that works while half of it takes documents down. */
    "function orphanUpdate(e){ var i = e.n, j = ++i, k = i--, n = 0;"
    " for (var m = e.n; m--;) { n = n + 1; if (n > 2) break; }"
    " fetch('/api/orphan/update?n=' + n); }"
    /* AND A COERCE-THEN-COMPUTE BUILTIN WITH A GENERIC BODY — the clamp every bundle writes. §21.3.2.26
       Math.min ( ...args ) step 2 is `? ToNumber(arg)` for each argument and the declaration's own coercion is
       §7.1.1 ToPrimitive, which over an unknown is the identity, so the body's JS_ToFloat64 reached the
       conversion boundary. One unknown argument makes the whole result unknown (step 4's `number > highest` is
       undecided against a value nobody has), and the endpoint AFTER the clamp is the claim because an abort
       emits none. */
    "function orphanClamp(e){ var v = Math.max(0, Math.min(255, e.n));"
    " fetch('/api/orphan/clamp?v=' + v); }"
    "(async function(){ var c = await (await fetch('/api/config')).json(); fetch('/api/user?region=' + c.region); })();"   /* FETCH-AWAIT-RESULT: await a safe GET, then §6.4.3 json() over the host's bytes — the parsed body's field flows into a later endpoint as a concrete example */
    /* §6.4 clone(), which is how a caching or interceptor layer is written: copy the reply, read the copy, and
       still hand the original on. Both halves of the spec are asserted because both are the reason it exists —
       the clone reads INDEPENDENTLY (its own body-used latch, or the copy would be dead the moment the original
       was read), and cloning does NOT consume the original (or the layer would have nothing to pass along).
       The throw is SYNCHRONOUS on an already-read body, not a rejected promise, so a page's try/catch sees it
       where it was written. */
    "(async function(){ var r = await fetch('/api/config'); var c = r.clone();"
      " var a = await c.json(); var b = await r.json();"
      " var thrown = 'no'; try { r.clone(); } catch (e) { thrown = 'sync'; }"
      " fetch('/api/clonebody?copy=' + a.region + '&orig=' + b.region + '&used=' + thrown); })();"
    /* §6.4.1 arrayBuffer() / §6.4.2 bytes(): the reply as the BYTE SEQUENCE it is. They read the same reply two
       ways and report the SAME length and the SAME first byte ('{'), which is what says the two are one body and
       not two decodings of it — and a length that survives says the record carries one rather than recovering it
       with a strlen. Two clones, because reading is single-use. */
    "(async function(){ var r = await fetch('/api/config');"
      " var ab = await r.clone().arrayBuffer(); var by = await r.clone().bytes();"
      " fetch('/api/bodybytes?len=' + ab.byteLength + '&n=' + by.length + '&b0=' + by[0]"
      "   + '&kind=' + (ab instanceof ArrayBuffer) + (by instanceof Uint8Array)); })();"
    /* THE BODY-USED LATCH TIME-TRAVELS. The Response is awaited BEFORE the concolic branch, so ONE reply object
       is shared by both arms — and each then reads it. The latch is C state in the class opaque, which no
       property hook can see, so without a COW capture the first arm to read left the reply consumed for the
       other and the sibling's own first read threw. Both bodyADMIN and bodyPUBLIC present ⇒ each arm saw its own
       unread reply, which is the same isolation every shared JS slot already had. */
    "(async function(){ var r = await fetch('/api/config');"
      " var tag = cfg.admin ? 'bodyADMIN' : 'bodyPUBLIC';"   /* THE FORK — before either arm has read `r` */
      " var v = (await r.json()).region + '-' + tag;"        /* both arms read the SAME reply */
      " fetch('/api/bodyiso?v=' + v); })();"
    /* TWO METHODS, ONE ADDRESS — the request's IDENTITY, and the one thing no other probe here can see. The
       reply seam listed URLs and matched on URLs, so this page's two requests were ONE line on the join (the
       dedup dropped the POST) and ONE delivery filling BOTH entries: the POST's promise settled with the GET's
       reply and the page read a body the server produced for a request it never made. Every @H value and every
       @S verdict behind such a line came from that. `g=GET&p=POST` is what the pair-keyed seam produces and
       `p=GET` is what the URL-keyed one produced, so the two are told apart by the emitted record itself. */
    "(async function(){ var g = await fetch('/api/echo');"
      " var p = await fetch('/api/echo', { method: 'POST', body: 'x' });"
      " fetch('/api/verb?g=v' + g.headers.get('x-echo-method') + '&p=v' + p.headers.get('x-echo-method')); })();"
    /* §5 Headers. The RECORD fill is the conversion `fetch(u, {headers: {...}})` performs, so it is exercised
       through the interface that states it: a record init, then the members that read it back. The list keeps
       PAIRS — two `set-cookie` appends stay two entries and getSetCookie reads both — while `get` combines them
       per §2.2.2 Headers, which is the whole difference between a header list and a map. A name is lowercased on the
       way in, an absent header is null rather than "", and `set` replaces every entry with that name. */
    "(function(){ var h = new Headers({'X-Api-Key': 'k1', 'Accept': 'application/json'});"
      " h.append('Set-Cookie', 'a=1'); h.append('Set-Cookie', 'b=2');"
      " var sc = h.getSetCookie();"
      " h.append('X-Api-Key', 'k2');"                       /* append KEEPS both -> get joins them */
      " var joined = h.get('x-api-key');"                   /* case-insensitive: normalized on the way in */
      " h.set('X-Api-Key', 'k3');"                          /* set REPLACES every entry with that name */
      " fetch('/api/hdrs?acc=' + h.get('Accept') + '&sc=' + sc.length + ':' + sc[0] + ':' + sc[1]"
      "   + '&join=' + joined + '&set=' + h.get('X-Api-Key')"
      "   + '&has=' + h.has('accept') + h.has('nope') + '&miss=' + h.get('nope')); })();"
    /* THE INIT'S ARM IS DECIDED BY @@iterator, which is a READ of the page's object and not a type test. A
       plain bag with a PROXY in front of it still converts as a record — its `ownKeys` and its `get` are the
       page's code, driven as requests — and the proxied reads are what the record arm is made of. If the read
       of @@iterator were not part of the algorithm this would still pass; what it proves is that a trap-bearing
       init survives the conversion with its headers intact. */
    "(function(){ var seen = '';"
      " var p = new Proxy({'X-Trap': 'tv'}, { get: function(t, k){ seen = 'trapped'; return t[k]; } });"
      " var h2 = new Headers(p);"
      " fetch('/api/hdrproxy?v=' + h2.get('x-trap') + '&t=' + seen); })();"
    /* §5.2's iterable<>. Iteration is NOT the raw list: it SORTS by name and COMBINES each name's values, so
       two `x-a` appends are ONE entry — while `set-cookie` stays one entry per value, which is the whole reason
       the list keeps pairs. forEach hands the page (value, key) in that order, and its callback is the page's
       code driven as a request. `for...of` over the Headers itself is `entries`, per §3.7.10. */
    "(function(){ var h = new Headers({'x-b': '2'});"
      " h.append('x-a', '1'); h.append('x-a', '9');"
      " h.append('Set-Cookie', 'c1'); h.append('Set-Cookie', 'c2');"
      " var ks = Array.from(h.keys()).join('|');"
      " var vs = Array.from(h.values()).join('|');"
      " var es = ''; for (var e of h) { es += e[0] + '=' + e[1] + ';'; }"
      " var fe = ''; h.forEach(function(v, k, t){ fe += k + ':' + v + ';'; if (t !== h) fe += 'BADTHIS'; });"
      " fetch('/api/hdriter?k=' + ks + '&v=' + vs + '&e=' + es + '&f=' + fe); })();"
    /* §5.1's SEQUENCE arm, which Web IDL picks because the init is ITERABLE. It is the iterator protocol twice
       over — once for the pairs, once for each pair's two items — so a GENERATOR init proves it is the protocol
       and not an array walk, and a MAP proves the same for the shape that is iterable without being an array.
       A pair that is not exactly two items is a TypeError, and null is not "no init": HeadersInit is a union of
       object types that Web IDL does not make nullable. */
    "(function(){"
      " function* g(){ yield ['x-gen', 'g1']; yield ['x-gen', 'g2']; }"
      " var a = new Headers(g()).get('x-gen');"
      " var b = new Headers(new Map([['x-map', 'm1']])).get('x-map');"
      " var c = new Headers([['x-arr', 'a1']]).get('x-arr');"
      " var bad = 'no'; try { new Headers([['only-one']]); } catch (e) { bad = 'threw'; }"
      " var nul = 'no'; try { new Headers(null); } catch (e) { nul = 'threw'; }"
      " fetch('/api/hdrseq?g=' + a + '&m=' + b + '&a=' + c + '&bad=' + bad + '&nul=' + nul); })();"
    /* THE RECORD'S DEDUP IS A DIFFERENT EQUIVALENCE THAN THE HEADER LIST'S, and that is the whole statement.
       Web IDL §3.2.23 converts `record<ByteString, ByteString>` with "Let typedKey be key converted to an IDL
       value of type K … Set result[typedKey] to typedValue" — K is ByteString, which does not case-fold, so
       `X-Rec` and `x-rec` are TWO entries of the record and the fill sees both. §5.1's fill then "append(key,
       value) to headers" for each, and §2.2.2 Headers' get returns "the values of all headers … separated from each
       other by 0x2C 0x20": "r1, r2", over ONE name in the list. A replace loop matching the LOWERCASED name
       stood in the fill and answered "r2" — one pair silently discarded, which for this tool is a header the
       report would not carry. */
    "(function(){ var h = new Headers({'X-Rec': 'r1', 'x-rec': 'r2'});"
      " fetch('/api/hdrrec?v=' + h.get('x-rec') + '&k=' + Array.from(h.keys()).join('|')); })();"
    /* HTML §4.10.22.8's ESCAPE, on the one input that shows it: a field name is written INSIDE QUOTES in the
       part's Content-Disposition, so "the result of the encoding … must be escaped by replacing any 0x0A (LF)
       bytes with the byte sequence `%0A`, 0x0D (CR) with `%0D` and 0x22 (") with `%22`. The user agent must
       not perform any other escapes." Unescaped, `a"\r\nb` closes the quoted name and opens a second header
       line — the part boundary forged from inside the body — so BOTH halves are asserted: the escaped spelling
       is present AND the raw `a"` is absent, because a serializer that wrote the name twice would satisfy the
       first alone. The name already carries a CRLF pair, which is exactly what step 1's newline normalization
       leaves alone, so this statement is about the escape and nothing else. */
    "(function(){ var fd = new FormData(); fd.append('a\"\\r\\nb', 'v');"
      " new Response(fd).text().then(function(t){"
      "   fetch('/api/mpesc?esc=' + (t.indexOf('name=\"a%22%0D%0Ab\"') >= 0)"
      "     + '&raw=' + (t.indexOf('a\"') >= 0)); }); })();"
    /* THE TRANSPORT REQUIREMENT REACHES THE SURFACE. `init.headers` is read and converted, and the endpoint
       carries what the request needs — which is the half of "usable" the @H surface never had. The
       Authorization value is built out of `state`, so it is a CONCOLIC and reports its SHAPE: the `{hole}` is
       what tells a reviewer this one is a runtime value to supply, while the two literal headers are the real
       strings the code computed. Inventing a token for the first would be a fabricated observation. */
    "fetch('/api/needsauth', { method: 'POST', headers: {"
      " 'Authorization': 'Bearer ' + state.token,"
      " 'X-Api-Version': '2024-11-01',"
      " 'Content-Type': 'application/json' } });"
    /* THE OTHER TWO PLACES A REQUEST CARRIES A VALUE — the PATH and the BODY. The surface named only the query
       string, so a templated address was one opaque row and a POST's payload was nothing at all; every
       consumer branched on a `location` field no producer wrote, which is why the path-parameter registration
       and the whole request-body schema had never run.
       This row's path hole is SYMBOLIC (server-injected `state`), so the param is named and carries NO example
       — the record says the code did not compute one rather than inventing a number for it. Its body is real
       JSON, so `title` and `count` arrive as body params with the literals the code computed. */
    "fetch('/v1/users/' + state.id + '/posts', { method: 'POST',"
      " headers: { 'Content-Type': 'application/json' },"
      " body: JSON.stringify({ title: 'firstPost', count: 3 }) });"
    /* AND THE ALIGNED EXAMPLE, which is the half a shape alone cannot show. HTML §6.2 "Page visibility"'s
       state is CONCOLIC (a branch on it still forks both worlds) AND carries the value this document has,
       so the URL's shape and its computed example line up segment for segment and the path param gets
       `visible` — a value the code determined, never a solve. The query param beside it proves the three
       kinds coexist on one record. */
    "fetch('/v1/vis/' + document.visibilityState + '/reports?deep=1');"
    "(async function(){ var p = new Promise(function(res){ Promise.resolve().then(function(){ res('lazyRegion'); }); }); var c = await p; fetch('/api/lazy?r=' + c); })();"   /* PENDING await: the promise resolves LATER via a microtask (stands in for the network) -> park + resume */
    "var _spr; var _sp = new Promise(function(r){ _spr = r; }); _sp.then(function(v){ fetch('/api/shared?v=' + v); }); _spr(state.beta ? 'shBETA' : 'shSTABLE');"   /* PROBE: _sp created BEFORE the state.beta snapshot fork -> shared; settled per-flow. If promise state is NOT per-flow COW, only ONE value survives (contamination) */
    "if (state.gamma) { delete delObj.k; } fetch('/api/tok?t=' + (delObj.k ? delObj.k : 'wasDeleted'));"   /* DELETE time-travel: the gamma-true flow deletes delObj.k; the gamma-false flow must STILL see keepVAL (the delete is per-flow) */
    /* THE SAME DELETE ON THE GLOBAL, WHICH IS A DIFFERENT OBJECT KIND AND WAS THE ONE THIS DOCUMENT NEVER MADE.
       Every `delete` above is on an ordinary object; the global is EXOTIC (the Window class's supported-index
       hooks), so a delete of a name it does not own misses the shape and lands in the exotic dispatch — and a
       creation this flow then removes is the same arrival by the other route. Both are what a real bundle does
       (`delete globalThis.__loggedIn` sits in a logout path), and both reach the swap as a slot removal that
       must consult neither 10.1.10.1 step 3's [[Configurable]] nor the class's own handler. gdSOLE on BOTH arms
       is the two-sided claim: each arm read back ITS OWN global creation, and neither absent-slot delete left
       anything behind for the other to find. */
    "delete globalThis.gdGone; globalThis.gdTag = cfg.admin ? 'gdADMIN' : 'gdPUBLIC';"
    " fetch('/api/gdel?v=' + globalThis.gdTag + (globalThis.gdGone === undefined ? 'gdSOLE' : 'gdLEAK'));"
    " delete globalThis.gdTag;"
    "function floc(){ var o = { x: 'base' }; if (cfg.admin) { o.x = 'flocADMIN'; } else { o.x = 'flocPUBLIC'; } fetch('/api/floc?x=' + o.x); } floc();"   /* FLOW-LOCAL post-fork isolation: o is created inside floc() BEFORE the concolic fork; the snapshot shares o across siblings, so each arm's o.x mutation must be captured per-flow (cow_delta_fork forked=1). Both flocADMIN and flocPUBLIC ⇒ no cross-flow leak — settles whether the 'snapshot shares flow_local' comment is real or stale. */
    "var acc = 0; for (var i = 0; i < 15; i++) { acc = acc + i; }"   /* a real LOOP (back-edges) so the quantum fires: a flow yields MID-LOOP and the scheduler interleaves parked sibling flows, exercising the COW+decide+pins swap */
    "function* gen(n){ var s=0; for(var i=0;i<n;i++){ s=s+i; } yield s; yield s*2; }"   /* GENERATOR body with a LOOP: .next() runs the body on the tramp chain, the loop preempts the base flow */
    "var git = gen(2000); fetch('/api/gen?a=' + git.next().value + '&b=' + git.next().value);"
    "function* gg(){ for(var i=0;i<10;i++) yield i*i; } var gsum=0; for(const x of gg()){ gsum = gsum + x; } fetch('/api/genforof?s=' + gsum);"
    "function* gt(){ try { yield 1; } catch(e){ yield 'c:'+e; } } var gi = gt(); gi.next(); fetch('/api/genthrow?v=' + gi.throw('X').value);"   /* direct .next()/.next() -> 1999000, 3998000 */
    "function* gp([a,b,c]){ yield a+b+c; } fetch('/api/genparam?v=' + gp([10,20,30]).next().value);"   /* PARAM-DESTRUCTURING generator: the array destructuring iterates at CREATION on the tramp chain (do_generator_create_tramp) */
    "var _fe=0; [10,20,30].forEach(function(x){ var t=0; for(var i=0;i<50;i++) t++; _fe += x + t; }); fetch('/api/foreach?s=' + _fe);"   /* forEach callback with a LOOP: the callback runs on the tramp chain (do_array_iter_tramp), so its back-edges PARK the base flow and resume — never drive-to-completion -> 60+150=210 */
    "[1,2].forEach(function(e){ var w = cfg.admin ? 'feADMIN' : 'fePUBLIC'; fetch('/api/fefork?e=' + e + w); });"   /* CONCOLIC branch INSIDE a forEach callback: forks DEEP (chain base->iter-callback). clone_deep_flow must clone the callback frame's cont_state (JSArrayEvery) so the sibling continues the iteration independently -> both feADMIN and fePUBLIC over elements 1 AND 2 */
    "var pfa = []; if (cfg.admin) { pfa.push('pushA'); } else { pfa.push('pushB'); } fetch('/api/pushfork?a=' + pfa.join(','));"   /* SHARED-ARRAY append isolation: pfa is created before the concolic fork (shared by the snapshot); each arm's push must be COW-isolated via the fast-array-append capture (element slot, removed by truncate-to-index on unapply) -> EXACTLY 'pushA' and 'pushB', never the contaminated 'pushA,pushB' */
    "var owa = ['base']; if (cfg.admin) { owa[0] = 'owA'; } else { owa[0] = 'owB'; } fetch('/api/owfork?a=' + owa[0]);"   /* SHARED-ARRAY OVERWRITE isolation: owa[0] exists before the fork; each arm overwrites the SAME in-bounds element. The overwrite fast-path (set_value on values[idx]) must capture the baseline -> EXACTLY 'owA' and 'owB', never one arm seeing the other's value */
    "fetch('/api/mapfork?r=' + [1,2].map(function(e){ return (cfg.admin ? 'mA' : 'mP') + e; }).join('-'));"   /* CONCOLIC branch inside a MAP callback: the deep-fork clones the JSArrayEvery cont_state (shared ret array, COW-isolated per arm via fast-array capture). cfg.admin is config (symbolic, forks per element) -> 4 clean combos incl mA1-mA2 and mP1-mP2, none with a dropped element */
    "fetch('/api/arrmap?s=' + [1,2,3,4,5].map(function(x){ return x*2; }).filter(function(x){ return x>4; }).join(','));"   /* map + filter through the step coroutine -> 6,8,10 */
    "var _fc=0; Array.prototype.forEach.call([5,6], function(x){ var t=0; for(var i=0;i<20;i++) t++; _fc += x + t; }); fetch('/api/fecall?s=' + _fc);"   /* .call is CALL-SITE-RESOLVED: unwrapped at the operator site (do_forward_call) and re-dispatched into the array-iter coroutine, so the looping callback still PARKS the base -> 11+40=51 */
    "fetch('/api/reduce?s=' + [1,2,3,4].reduce(function(a,x){ var t=0; for(var i=0;i<10;i++) t++; return a + x*t; }, 0));"   /* reduce callback with a LOOP: same coroutine shape (do_array_reduce_tramp), the accumulator rides the state -> 100 */
    "(async function(){ await Promise.resolve(1); var s=0; for(var i=0;i<30;i++) s+=i; fetch('/api/postawait?s=' + s); })();"   /* loop AFTER an await: the post-await body is a reaction-driven CONTINUATION, resumed as its OWN flow (js_async_function_resume_as_flow) so its loop PARKS and re-enters the job pump -> 435 */
    "async function* agp(x = (function(){ throw 'pthrow'; })()) { yield 1; }"
    "try { agp(); } catch (e) { fetch('/api/agthrow?e=' + e); }"   /* PARAM-BINDING THROW during async-generator creation: the object does not exist yet and the frame's buffers belong to its STATE, so this must take the agen-create exception path (not the generic pop) -> pthrow */
    "var _om = { async m(x){ var s=0; for(var i=0;i<15;i++) s+=i; return x+s; } };"
    "_om.m(5).then(function(v){ fetch('/api/asyncmethod?v=' + v); });"   /* ASYNC METHOD (obj.m(), tramp_first==-2): its operands are [this,func,args] — do_async_tramp_call must pop by tramp_first, not a hardcoded -1, or `this` leaks and the caller's sp lands a slot high (heap corruption) -> 110 */
    "function Ctr(n){ var s=0; for(var i=0;i<n;i++) s+=i; this.v=s; } fetch('/api/ctor?v=' + new Ctr(20).v);"   /* do_construct_tramp: a plain-function constructor body loop PARKS the base flow instead of C-recursing through JS_CallConstructorInternal -> 190 */
    "var _go = { get big(){ var s=0; for(var i=0;i<25;i++) s+=i; return s; } }; fetch('/api/getter?v=' + _go.big);"   /* BYTECODE GETTER via OP_get_field on the tramp chain: the getter body loop PARKS the base flow (do_tramp_call via tramp_bytecode_getter) instead of C-recursing through JS_GetPropertyInternal -> 300 */
    "var _pf = ({poisoned}) => { return 1; }; var _pc = 0; try { _pf({ get poisoned(){ throw 7; } }); } catch(e){ _pc = e; } fetch('/api/dstrthrow?v=' + _pc);"   /* getter throws during PARAM DESTRUCTURING -> nested tramp unwind */
    "var _ga = { get bracket(){ var t=0; for(var i=0;i<30;i++) t+=i; return t; } }; fetch('/api/getarr?v=' + _ga['bracket']);"   /* GETTER via OP_get_array_el (computed key): tramp_bytecode_getter after ToPropertyKey routes the getter-body loop onto the chain -> 435 */
    "var _so = { _x:0, set load(v){ var s=0; for(var i=0;i<v;i++) s+=i; this._x = s; } }; _so.load = 20; fetch('/api/setter?v=' + _so._x);"   /* BYTECODE SETTER on the tramp chain: the setter-body loop PARKS the base flow (do_tramp_call via tramp_bytecode_setter, CONT_SETTER discards the result) -> 190 */
    "var _sa = { _y:0, set slot(v){ var s=0; for(var i=0;i<v;i++) s+=i; this._y = s; } }; _sa['slot'] = 25; fetch('/api/setarr?v=' + _sa._y);"   /* SETTER via OP_put_array_el (computed key): tramp_bytecode_setter routes the setter-body loop onto the chain -> 300 */
    "function _ap(a,b,c){ var s=0; for(var i=0;i<a;i++) s+=i; return s+b+c; } fetch('/api/apply?v=' + _ap.apply(null, [20, 5, 3]));"   /* f.apply(this, arr) with a looping body -> do_apply_tramp: args spread from arr into f's frame -> 190+5+3=198 */
    "function _at(){ throw 9; } var _atc=0; try { _at.apply(null, [1,2]); } catch(e){ _atc=e; } fetch('/api/applythrow?v=' + _atc);"   /* apply target THROWS -> nested tramp unwind -> 9 */
    "function _bf(a,b,c){ var s=0; for(var i=0;i<a;i++) s+=i; return s+b+c; } var _bb = _bf.bind(null, 20, 5); fetch('/api/bind?v=' + _bb(3));"   /* f.bind(this, boundArgs)(callArgs) with a looping body -> do_bound_tramp assembles bound++call args -> 190+5+3=198 */
    "function _bt(){ throw 4; } var _btb = _bt.bind(null); var _btc=0; try { _btb(); } catch(e){ _btc=e; } fetch('/api/bindthrow?v=' + _btc);"   /* bound target THROWS -> nested tramp unwind -> 4 */
    "function _sf(a,b,c){ var s=0; for(var i=0;i<a;i++) s+=i; return s+b+c; } var _sarr=[20,5,3]; fetch('/api/spread?v=' + _sf(..._sarr));"   /* f(...arr) SPREAD with a looping body -> OP_apply routes into do_apply_tramp -> 198 */
    "function _rf(a,b,c){ var s=0; for(var i=0;i<a;i++) s+=i; return s+b+c; } fetch('/api/reflectapply?v=' + Reflect.apply(_rf, null, [20,5,3]));"   /* Reflect.apply(target, this, argsList) with a looping body -> do_apply_tramp -> 198 */
    "var _sa=[3,1,2,5,4]; _sa.sort(function(x,y){ var s=0; for(var i=0;i<5;i++) s++; return x-y; }); fetch('/api/sort?v=' + _sa.join(String.fromCharCode(44)));"   /* arr.sort(cmp) with a looping comparator -> CONT_SORT merge-sort coroutine, comparator body loop PARKS the base -> 1,2,3,4,5 */
    "var _sta=[3,1,2]; var _stc=0; try { _sta.sort(function(x,y){ throw 8; }); } catch(e){ _stc=e; } fetch('/api/sortthrow?v=' + _stc);"   /* comparator THROWS -> sort-state freed on unwind -> 8 */
    "var _jr = JSON.parse(String.fromCharCode(123,34,97,34,58,53,44,34,98,34,58,55,125), function(k,v){ if(typeof v===String.fromCharCode(110,117,109,98,101,114)){ var s=0; for(var i=0;i<v;i++) s+=i; return s; } return v; }); fetch('/api/jsonrevive?v=' + (_jr.a + _jr.b));"   /* JSON.parse reviver with a looping body -> CONT_JSON_REVIVE explicit-stack walk -> a:10 + b:21 = 31 */
    "var _jt=0; try { JSON.parse(String.fromCharCode(123,34,120,34,58,49,125), function(k,v){ if(k===String.fromCharCode(120)) throw 3; return v; }); } catch(e){ _jt=e; } fetch('/api/jsonthrow?v=' + _jt);"   /* reviver THROWS -> walk state freed on unwind -> 3 */
    "let _lex = 7; const _kex = 8; class _Cex { g(){ return _lex + _kex; } } fetch('/api/lexbind?v=' + new _Cex().g());"   /* TOP-LEVEL LEXICAL bindings (let/const/class) define into the SHARED global_var_obj; JS_DefineGlobalVar now COW-captures the creation so it is per-flow, not leaked into snapshot siblings (whose re-definition would else throw redeclaration and kill the sibling + its downstream forks � a silently dropped @H arm) -> 15 */
    /* GENERATOR-BODY CONCOLIC FORK — placed LAST so its arms don't multiply the downstream fixture subtree (a
       branch's cost is (upstream flows) × (downstream work); at the tail the downstream work is just the fetch). */
    "function* ggf(){ if (cfg.admin) { yield 'ggADMIN'; } else { yield 'ggPUBLIC'; } } var ggi = ggf(); fetch('/api/genfork?v=' + ggi.next().value);"   /* CONCOLIC branch INSIDE a synchronously-driven generator body: the .next() drive runs the body on the tramp chain, so the branch snapshot-forks the tramp-driven generator activation (the named nested-activation hold-out) -> both ggADMIN and ggPUBLIC, never a DFAIL */
    "function* g2f(){ var a = cfg.admin ? 'A' : 'P'; var b = state.beta ? 'X' : 'Y'; yield 'g2:'+a+b; } var g2i = g2f(); fetch('/api/gen2fork?v=' + g2i.next().value);"   /* TWO concolic branches in ONE generator body: the FIRST fork installs a per-flow gen_data clone on the sibling; when that sibling hits the SECOND branch it re-forks the SAME generator -> exercises cow_delta_add_gendata's dedup-REPLACE (keep the original base, swap the clone). All four combos g2:AX/AY/PX/PY ⇒ each fork advanced its own generator state */
    "function* gof(){ if (cfg.admin) { yield 'ofA'; } else { yield 'ofP'; } } var ofr=''; for (const x of gof()) { ofr += x; } fetch('/api/genofork?v=' + ofr);"   /* FOR-OF generator-body concolic fork: the generator is driven by for-of (its object lives on the caller stack, not the tramp frame), so clone_deep_flow recovers it from caller_sp[forof_off] to record the per-flow gen_data swap -> both ofA and ofP */
    "function* aff(){ if (cfg.admin) { yield 'afA'; } else { yield 'afP'; } } fetch('/api/afromfork?v=' + Array.from(aff())[0]);"   /* Array.from(GEN) consumer fork: the gen body branches while CONSUMED by Array.from on the tramp (CONT_ITER_CONSUME), so clone_deep_flow's gen-branch clones the JSIterConsume state -> both afA and afP */
    "function* spf(){ if (cfg.admin) { yield 'spA'; } else { yield 'spP'; } } fetch('/api/spreadfork?v=' + [...spf()][0]);"   /* [...GEN] spread consumer fork: same CONT_ITER_CONSUME machinery, SPREAD sink (append to the literal's array), forks mid-consume -> both spA and spP */
    "var _sad = new Set(); if (cfg.admin) { _sad.add('sadA'); } else { _sad.add('sadP'); } fetch('/api/setaddfork?v=' + [...(_sad)][0]);"   /* SHARED-SET record isolation: _sad is created before the concolic fork; each arm's Set.add must be COW-isolated via the map_add capture (record removed by JS_MapDeleteRecord on unapply) -> EXACTLY 'sadA' and 'sadP', never a contaminated set holding both */
    "function* sef(){ if (cfg.admin) { yield 'seA'; } else { yield 'seP'; } } fetch('/api/setfork?v=' + [...new Set(sef())][0]);"   /* new Set(GEN) consumer fork: the Set consumer (CONT_ITER_CONSUME, SET sink) forks mid-consume; now fork-SAFE via the map_add COW capture -> both seA and seP */
    "var _mm = new Map([['k','base']]); if (cfg.admin) { _mm.set('k','mmA'); } else { _mm.delete('k'); } fetch('/api/mapmutfork?v=' + (_mm.has('k') ? _mm.get('k') : 'gone'));"   /* SHARED-MAP overwrite/delete isolation: _mm is created before the fork; one arm OVERWRITES 'k', the other DELETES it. The map_mutate undo-log capture (unapply restores the old value / re-adds) keeps them per-flow -> EXACTLY 'mmA' and 'gone', never cross-contaminated */
    "(async function(){ function* afsf(){ if (cfg.admin) { yield 'afsA'; } else { yield 'afsP'; } } var out=[]; for await (var x of afsf()) { out.push(x); } fetch('/api/afsfork?v=' + out[0]); })();"   /* for-await(GEN) consumer fork: the sync gen body branches while driven by the async-from-sync consumer (CONT_ASYNC_FROM_SYNC) on the tramp — clone_deep_flow clones the JSAsyncFromSync state with a FRESH wrapper promise per arm -> both afsA and afsP */
    "function* paf(){ if (cfg.admin) { yield Promise.resolve('pafA'); } else { yield Promise.resolve('pafP'); } } Promise.all(paf()).then(function(a){ fetch('/api/paffork?v=' + a[0]); });"   /* Promise.all(GEN) consumer fork at index==0: the gen branches during the FIRST .next() before any element .then is attached (CONT_PROMISE_ALL) — clone_deep_flow clones the JSPromiseAll aggregate fresh per arm -> both pafA and pafP */
    "function* paf2(){ yield Promise.resolve('p0'); if (cfg.admin) { yield Promise.resolve('pf2A'); } else { yield Promise.resolve('pf2P'); } } Promise.all(paf2()).then(function(a){ fetch('/api/paf2fork?v=' + a[0] + '-' + a[1]); });"   /* Promise.all(GEN) consumer fork at index>0: the gen yields element 0 THEN branches (fork during .next() #2, index==1). The retained pre-fork element wrapper (p0) is RE-ATTACHED to the sibling aggregate -> BOTH arms resolve a[0]=='p0' AND their own a[1] (p0-pf2A and p0-pf2P) */
    "var _rr = 'x1y2'.replace(/\\d/g, function(d){ return cfg.admin ? 'rrA'+d : 'rrP'+d; }); fetch('/api/rerepfork?v=' + _rr);"   /* @@replace CALLBACK fork (JSReRep): an OBJECT searchValue dispatches to RegExp.prototype[@@replace], whose machine holds THREE things across each callback — the collected match array, the spec's captures List in its own block, and the StringBuffer accumulator with nextSourcePosition. The replacer branches on opaque state at the FIRST of two matches, so the sibling must get its own copy of all three or the second match lands in the wrong accumulator. Pure paths xrrA1yrrA2 and xrrP1yrrP2 both present => each arm substituted BOTH matches into its OWN buffer. */
    "var _red=[1,2].reduce(function(acc,x){ return acc + (cfg.admin ? 'rA' : 'rP') + x; }, 'r:'); fetch('/api/redfork?v=' + _red);"   /* reduce ACCUMULATOR fork (CONT_ARRAY_REDUCE): the reducer branches on opaque state mid-fold, so clone_deep_flow clones the JSArrayReduce accumulator per arm. The pure paths r:rA1rA2 and r:rP1rP2 both present ⇒ each arm threaded its OWN accumulator across both elements (never a shared/contaminated acc) */
    "var _tpj={toString:function(){ return cfg.admin ? 'tpA' : 'tpP'; }}; fetch('/api/toprimfork?v=' + ['x','y'].join(_tpj));"   /* MACHINE-MODE ToPrimitive fork: 23.1.3.18 Array.prototype.join coerces its SEPARATOR, so §7.1.1 ToPrimitive runs the page's toString ON THE TRAMP with the JSArrayJoin machine as the JSToPrim's OUTER requester — a machine that owns no frame and is reachable only through `outer`. The branch is INSIDE that toString. Both pure paths xtpAy and xtpPy present ⇒ tramp_cont_relink_outer cloned the machine as well as the sequence, so each arm finished its own join with its own separator (a shared machine would leave one arm's separator in the other's buffer, or free the cursor twice) */
    "function* gcf(){ if (cfg.admin) { yield 'gcA'; } else { yield 'gcP'; } } var gci=gcf(); fetch('/api/gcallfork?v=' + gci.next.call(gci).value);"   /* generator .next() driven via .call BYPASS: gci.next.call(gci) reshapes at do_forward_call to the [this=gen, f=next] shape and is now routed onto do_generator_tramp (not the js_generator_next drive-to-completion). The body branches -> both gcA and gcP, never a DFAIL */
    "function* gapf(){ if (cfg.admin) { yield 'gapA'; } else { yield 'gapP'; } } var gapi=gapf(); fetch('/api/gapplyfork?v=' + gapi.next.apply(gapi, []).value);"   /* generator .next() via Function.prototype.apply BYPASS: reshaped at OP_call_method to [this=gen, next, arg0] and routed onto do_generator_tramp -> both gapA and gapP, never drive-to-completion */
    "function* graf(){ if (cfg.admin) { yield 'graA'; } else { yield 'graP'; } } var grai=graf(); fetch('/api/grefapplyfork?v=' + Reflect.apply(grai.next, grai, []).value);"   /* generator .next() via Reflect.apply BYPASS: [Reflect,apply,target,this,argsList] reshaped to [this=gen, next, arg0] and routed onto do_generator_tramp -> both graA and graP */
    /* A SYNCHRONOUS READ THE HOST MUST ANSWER — the shape `iframe.contentWindow.document.body` has. The value
       arrives at the CALL SITE, on the next line, with no promise anywhere: between the two the flow suspended,
       the scheduler ran other flows, the host answered, and this flow resumed. Inside a LOOP so it happens
       repeatedly, and the concatenation is what proves each answer reached the call that asked for it rather
       than an earlier or later one. */
    "var _hr=''; for (var _i=0;_i<3;_i++) { _hr += hostRead('hr'+_i); } fetch('/api/hostreq?v=' + _hr);"
    /* AND ONE ON EACH SIDE OF A FORK. A blocked flow that forks must RE-ISSUE its request under the sibling's
       own world — sharing the id would deliver one answer into two call sites in two contradictory worlds. */
    "fetch('/api/hostreqfork?v=' + hostRead(cfg.admin ? 'hrA' : 'hrP'));"
    /* §7.4: a child navigable. `open()` hands back a WindowProxy for a document in ANOTHER instance AT ITS OWN
       CALL SITE — the child's name is minted here, so there is nothing to ask and nothing to suspend for. */
    "var _w = open(); fetch('/api/navopen?v=' + (_w ? 'proxy' : 'null'));"
    /* §7.2.5.1's SAME-ORIGIN CHECK. A popup at ANOTHER origin exposes the fixed cross-origin list and nothing
       else: `closed` answers, `name` is a SecurityError. Both halves are the assertion — a filter that threw
       for everything would pass a test that only checked the throw. */
    "var _x = open('https://other.test/p'), _sop = '';"
    "try { _x.name; _sop += 'LEAKED'; } catch (e) { _sop += e.name; }"
    "fetch('/api/sop?v=' + _sop + ':' + (_x.closed === false ? 'closedok' : 'wrong'));"
    /* §7.2.5.1 ACROSS INSTANCES: the child's document lives in another instance, so this read SUSPENDS the
       flow, the peer answers, and the flow resumes with the value AT THE CALL SITE — `false`, not undefined. */
    /* The TYPES matter as much as the values: an answer that came back as the string "false" would satisfy a
       loose check and prove only that bytes moved. `=== false` and `=== 0` say the peer's value arrived as
       itself. */
    /* §4.8.5: INSERTING an <iframe> creates a child navigable IN THE INSERTION STEPS, so `contentWindow`
       answers ON THE LINE AFTER THE APPEND — no task, no microtask, no await. The read is deliberately in the
       same statement sequence as the append: a create that had to ask the host could not satisfy it, which is
       exactly the shape the spec files assert (`document.body.appendChild(frame).contentWindow`). Reading
       THROUGH the proxy is what suspends, and the peer answers, so `closed` arrives as a real boolean. */
    /* AND THE NAVIGABLE'S OWN STATE IS ANSWERED HERE, not by the peer: self/window/frames are this navigable's
       proxy by definition, `parent`/`top` are the creator, `opener` is null for a nested one, and `name` is the
       element's `name` ATTRIBUTE — which the removal below then empties while leaving the attribute alone. */
    "var _if = document.createElement('iframe'); _if.setAttribute('name','fr');"
    "document.body.appendChild(_if); var _cw = _if.contentWindow;"
    "var _ifok = !!_cw && _cw.self === _cw && _cw.window === _cw && _cw.frames === _cw &&"
    " _cw.globalThis === _cw && _cw.parent === window && _cw.top === window && _cw.opener === null &&"
    " _cw.closed === false && _cw.name === 'fr';"
    /* AND ONE READ THAT REACHES THE ACTIVE DOCUMENT, which is what makes the removal below a RECLAMATION probe
       rather than a flag probe. Every member above is answered from the navigable's own record, so none of them
       builds anything: without this line the srcless child stays unmaterialized for the whole fixture and the
       destroy has no realm to let go of. `document` forwards to the child's Window, so the initial about:blank
       Document and its realm exist from here on — ONE PER FLOW, which is the shape the ceiling is made of. */
    "_ifok = _ifok && !!_cw.document && _cw.document !== document;"
    /* §4.8.5's REMOVING STEPS: the element loses its navigable, and the proxy the page still holds stays the
       same object while reporting a destroyed one. */
    "document.body.removeChild(_if);"
    "_ifok = _ifok && _if.contentWindow === null && _cw.closed === true && _cw.name === '' &&"
    " _cw.self === _cw && _if.getAttribute('name') === 'fr';"
    /* AND THE PROXY THE PAGE IS STILL HOLDING HAS NO ACTIVE DOCUMENT — HTML §7.5.10's own note ("even after
       destruction, the Document object itself might still be accessible to script, in the case where we are
       destroying a child navigable") is about a reference the PAGE kept, and `_cw` is exactly that reference.
       Step 9 nulled the navigable's active document, so this read must not reach one AND must not build one:
       a proxy whose realm slot is empty because it was released reads identically to one that has never been
       materialized, and only the destroyed flag tells them apart (window_proxy.c's proxy_realm asserts it).
       The `undefined` is §7.2.3's own surface answering a name it does not carry, which is this engine's
       pre-existing answer for every through-read on a closed navigable and not a value this probe chose. */
    "_ifok = _ifok && _cw.document === undefined;"
    "fetch('/api/iframenav?v=' + (_ifok ? 'ifnav' : 'wrong'));"

    /* WHAT IS NOT PROBED HERE, AND WHY IT IS NOT A CHOICE: an iframe with a REAL `src`. It would exercise the
       whole of §7.4 step 14 — the load job asks the host for the address, PARKS, resumes with the response and
       runs the child's own scripts — and about:blank reaches none of that, because it has no response to fetch.
       IT IS NOT WHAT MAKES THE REALM COUNT CLIMB, though, and that distinction is what the two lines added
       above are for. A srcless navigable that something READS THROUGH is materialized exactly as a src'd one
       is, so the probe now costs ONE CHILD REALM PER FLOW with no load in it — the ceiling's shape, isolated
       from the fetch. What the removal then exercises is §7.5.10 step 9: the navigable lets go of the Document,
       which is the one counted reference this engine holds to a child realm, and the realm is garbage as soon
       as no world still names it (core/frame/window_proxy.h). READ `childRealms` ON THE @HEAP LINE: it must
       track the flows that are currently BETWEEN the read and the removal, not the flows that have ever run.
       A count equal to the flow count says a world is still holding one — the delta blobs of drained flows,
       or a collection that never ran — and neither is fixed by making this probe cheaper.
       The src'd path is not unexercised meanwhile: `node engine/wpt.mjs html/browsers` runs hundreds of src'd
       iframes at flow counts a realm-per-flow can still pay for. */

    /* HTML §8.6's TIMER TASK SOURCE, and §8.1.7's ordering around it — neither of which this fixture exercised
       at all, so `setTimeout` had no probe in the engine's own test despite being how a great deal of real code
       reaches the event loop. The ORDER is the assertion: a microtask checkpoint runs before the next task, so
       the `.then` continuation must observe its marker BEFORE the timer callback runs, and two timers with the
       same delay run in the order they were set. */
    "var _tk = '';"
    "setTimeout(function(){ _tk += 'B'; }, 0);"
    "setTimeout(function(){ _tk += 'C'; fetch('/api/timerfire?v=' + _tk); }, 0);"
    "Promise.resolve().then(function(){ _tk += 'A'; });"

    /* §8.7's STRING HANDLER, AND STEP 2's IDENTIFIER ACROSS THE TWO ARMS THAT HAND ONE BACK. The three rows
       above all pass a Function, so the arm of the timer initialization steps that returns a handle WITHOUT
       making an entry — the one that compiles the handler as a classic script — ran in no test in this tree.
       `strran` is that program having actually run; `next` is the two arms drawing from ONE per-global
       counter, which is what makes a handle name at most one timer. A second counter, or an arm that forgets
       to bump, gives two live timers one identifier and `clearTimeout` then clears the wrong one — a defect
       with no other symptom, since both calls still return a plausible number. */
    "var _tsh1 = setTimeout(\"fetch('/api/timerstr?v=strran');\", 0);"
    "var _tsh2 = setTimeout(function(){ fetch('/api/timerhandle?h=' +"
    " (_tsh2 === _tsh1 + 1 ? 'next' : 'reused')); }, 0);"

    /* §8.7 STEP 4 OVER AN UNKNOWN `timeout` — "if timeout is less than 0, then set timeout to 0" asked about
       a value nothing computed, which is a question with two real answers and therefore a FORK.
       IT IS THE FORK AND NOT THE TIMER THAT IS PROBED, and the `clearTimeout` is what keeps it that way. The
       arm that keeps the unknown writes an expiry no double can express, and FIRING one moves the event
       loop's virtual clock to a moment no arm has decided — a capability core/timing/event_loop.c does not
       have and whose absence timer_run_due names in full. Cleared, both arms reach the same end and what the
       row asserts is the thing that was missing: that a fork asked from inside §8.7's own algorithm has
       somewhere to put its sibling. As a plain JSCFunction it had none — the seam crashed at the fork, naming
       the predicate, so this ONE LINE took the whole document down before any of it ran. `unkdelay` is
       therefore a row that reads 0 when the members stop being step machines, and it reads 0 by ABORTING. */
    "var _t4 = setTimeout(function(){ fetch('/api/unkdelay/fired'); }, state.d);"
    "clearTimeout(_t4);"
    "fetch('/api/unkdelay?v=' + (typeof _t4 === 'number' ? 'forked' : 'nohandle'));"

    /* THE SAME READ FROM INSIDE A JOB. A `.then` handler is a queued reaction, and a cross-document read
       SUSPENDS — so this exercises a step machine that parks on the host while it is the root of a job rather
       than reached from a bytecode frame. Without the scheduler reporting that flow host-owed, it resumes and
       parks forever while every turn looks like progress, the host is never asked, and the run livelocks. */
    "Promise.resolve().then(function(){"
    " fetch('/api/xdocjob?v=' + (_w.closed === false ? 'jobread' : 'wrong')); });"
    "fetch('/api/xdocread?v=' + (_w.closed === false && _w.length === 0 &&"
    " typeof _w.closed === 'boolean' && typeof _w.length === 'number' ? 'xread' : 'wrong'));"

    /* INDEXED DATABASE FROM THE PAGE'S OWN DOOR — `indexedDB.open` through to `success`, and every part of
       the marker is a different sentence of the standard.
       `0:1` is §4.2's IDBVersionChangeEvent carrying §5.7 step 7's OLD VERSION and the version being opened,
       which is what every migration in every bundle branches on. `versionchange` is §4.10's mode getter over
       the transaction §5.7 step 2 created and §2.8.1's "the transaction of an open request is null unless an
       upgradeneeded event has been fired" made reachable. `pending` is §4.1's readyState while the `get`'s
       done flag is false — the request is returned BEFORE its operation runs, which is the whole of what
       "asynchronously execute" means. `99:done` is the value §6.1 stored under key 2 and §6.2 read back,
       delivered by §5.9's fire-a-success-event with the flag already set by §5.6 step 5.6.2.
       AND THE ORDER IS THE PROOF: `success` at the open request fires only after the upgrade transaction has
       FINISHED (§4.3's note says so, and §5.7 step 10 is what waits), and nothing calls `commit()` — §5.9
       step 9.3 found the request list empty once the `get` handler returned. `_r.result.version` is §4.4's
       version getter over the CONNECTION, which §5.1 step 9 set to the version that was opened. */
    /* AND §2.5's LIST KEY PATH, whose whole observable surface is the `keyPath` getter. `a+b` is §4.5's
       conversion of the store's list — Web IDL §3.2.21 makes it "a new Array object created as if by the
       expression []", so `plain` is the assertion that it is NOT a frozen array: `FrozenArray<T>` is a
       different parameterized type that neither the attribute's declaration (`readonly attribute any keyPath`)
       nor §4.5's prose names. `same` is the note's identity ("it returns the same object instance every time
       it is inspected"), which Web IDL §3.2.21 alone does not give since its own steps mint a new Array per
       conversion. And the SECOND handle, over a transaction the page opens after the upgrade, is the other
       half of that note: `perhandle` says two handles for one store answer with two Arrays, and the `a+b` that
       precedes it says the `push` into the first one reached no object store — "changing the properties of the
       object has no effect on the object store". `transaction('kp')` is also the first time §3.2.25's union
       takes its STRING arm from a page: a primitive string is not an Object, so §3.2.25 step 11.2's GetMethod
       is never reached and the store name is not iterated into 'k','p'.
       AND §7.1's IN-LINE KEYS, which is a `put` WITH NO KEY AT ALL — the store named it, and §7.1's walk reads
       it out of the value. `7:u-9` is the record coming back out under a key nothing passed: the value went in
       under `id.v`, the `get('u-9')` found it, and reading `.id.v` off the answer says the key was extracted
       from the value rather than moved out of it. `DataError` is §7.1's `failure` arm reported where §4.5 puts
       it — SYNCHRONOUSLY, out of `put` itself, where the page's own try/catch sees it, and not as a request
       that later fires an `error` event — for a value with nothing at the key path in a store with no key
       generator. Both are the shape every bundle that keeps records by id writes. */
    /* AND THE ROUND TRIP ITSELF, WHICH USED TO BE AN IN-C FIXTURE. §6.1 is a step machine now (its step 5
       drives §7.1 and therefore §7.4's array arm), so there is no C entry for it and these four claims run
       where a page runs them — which is strictly more than the C fixture asserted, because each goes through
       §5.6's request and §5.9/§5.10's event rather than calling the operation directly.
       `ConstraintError` is §6.1 step 2's no-overwrite refusal — what tells `add` from `put` — delivered as an
       `error` event that `preventDefault()` keeps from aborting the transaction (§5.10 step 9.3, which is the
       reason that event is cancelable at all). `first1` is §2.2's ordering read through §6.2: the records
       arrive 3, 1, 2 and the first in an unbounded-below range is the SMALLEST key, not the one that arrived
       first. `over21` is §6.1 step 3: a second put under one key REPLACES the record rather than filing a
       second one. `undoneAD` is §5.5 step 2 over the two RECORD changes, reached through a real `abort()`:
       the record the transaction ADDED is gone and the record it REMOVED is back with its own value.
       AND THE TAINT, which is the half a browser's own test suite cannot have: `location.hash` goes into a
       store and has to come back out still concolic, because a page keeps its session in here and reads it
       into gated code. §Attacker-sources names IndexedDB as a source; a store that de-tainted on the way in
       would answer with a plausible datum and delete the fork the read should produce. */
    "var _r = indexedDB.open('fixture', 1); var _idb = ''; var _k; var _rec = ''; var _tv; var _ix = '';"
    "_r.onupgradeneeded = function(e){"
    " var _s = _r.result.createObjectStore('s');"
    " _s.put(99, 2); var _g = _s.get(2);"
    " _idb += e.oldVersion + ':' + e.newVersion + ':' + _r.transaction.mode + ':' + _g.readyState;"
    " _g.onsuccess = function(){ _idb += ':' + _g.result + ':' + _g.readyState; };"
    " _k = _r.result.createObjectStore('kp', { keyPath: ['a', 'b'] });"
    " _idb += ':' + (Array.isArray(_k.keyPath) ? _k.keyPath.join('+') : 'notalist')"
    "       + ':' + (_k.keyPath === _k.keyPath ? 'same' : 'fresh')"
    "       + ':' + (Object.isFrozen(_k.keyPath) ? 'frozen' : 'plain');"
    " _k.keyPath.push('c');"
    " var _i = _r.result.createObjectStore('in', { keyPath: 'id.v' });"
    " _i.put({ id: { v: 'u-9' }, n: 7 });"
    " try { _i.put({ q: 1 }); _idb += ':nothrow'; } catch (_e) { _idb += ':' + _e.name; }"
    " var _ig = _i.get('u-9');"
    " _ig.onsuccess = function(){ _idb += ':' + _ig.result.n + ':' + _ig.result.id.v; };"
    /* THE ORDER OF THESE HANDLERS IS §5.6 STEP 5.1's, not this statement's: a transaction's requests are
       executed in the order they were placed, so the tokens accumulate in placement order. */
    " var _o = _r.result.createObjectStore('ord');"
    " _o.put(30, 3); _o.put(10, 1); _o.put(20, 2); _o.put(21, 2);"
    " var _oa = _o.add(11, 1);"
    " _oa.onerror = function(ev){ ev.preventDefault(); _rec += _oa.error.name; };"
    " var _ok = _o.getKey(IDBKeyRange.lowerBound(0));"
    " _ok.onsuccess = function(){ _rec += ':first' + _ok.result; };"
    " var _o2 = _o.get(2);"
    " _o2.onsuccess = function(){ _rec += ':over' + _o2.result; };"
    " var _t = _r.result.createObjectStore('t'); _t.put(location.hash, 1);"
    " var _tg = _t.get(1); _tg.onsuccess = function(){ _tv = _tg.result; };"
    /* §2.6's INDEX, created into an EMPTY store — the arm where the population request finds nothing to walk.
       The two indexes are the two arms of §6.1 step 5: `by_name` files ONE record whose key is the index key, and
       `by_tag` is multiEntry, so an ARRAY key files one record PER SUBKEY — which is what makes `multi2`
       load-bearing, both records carrying tag 'b'. `ref` is §6.3's retrieve-a-referenced-value (the index
       record's value is a store key, and the answer is the record THAT key names), `pk` is §6.3's
       retrieve-a-value (the store key itself), and `meta` is §4.6's attributes over the index handle. */
    " var _n = _r.result.createObjectStore('idx');"
    " var _x = _n.createIndex('by_name', 'name');"
    " var _mx = _n.createIndex('by_tag', 'tags', { multiEntry: true });"
    " _ix += _x.name + ':' + _x.keyPath + ':' + (_x.unique ? 'u' : 'nu')"
    "      + ':' + (_mx.multiEntry ? 'me' : 'nome')"
    "      + ':' + (_x.objectStore === _n ? 'own' : 'other')"
    "      + ':' + (_n.index('by_name') === _x ? 'same' : 'fresh');"
    " _n.put({ name: 'alice', tags: ['a', 'b'] }, 1);"
    " _n.put({ name: 'bob', tags: ['b', 'c'] }, 2);"
    " var _xg = _x.get('bob'); _xg.onsuccess = function(){ _ix += ':ref' + _xg.result.name; };"
    " var _xk = _x.getKey('bob'); _xk.onsuccess = function(){ _ix += ':pk' + _xk.result; };"
    /* AND §4.5's NOTE, THE HALF THAT IS NOT A FAILURE — the records go in FIRST and the index is created over
       a store that already holds them, so the only thing that can make these two retrievals answer is the
       POPULATION REQUEST walking the store's records through §6.1 step 5. `popben` is §6.3's
       retrieve-a-referenced-value over a record no `put` ever filed an index entry for, and `back2` is the
       multiEntry arm doing the same thing — one index record per subkey of a key extracted from a value the
       store was already holding, both records carrying tag 'x'. Created eagerly, as this file used to, both
       would answer with an empty index. */
    " var _pp = _r.result.createObjectStore('pop');"
    " _pp.put({ name: 'ann', tags: ['x'] }, 1);"
    " _pp.put({ name: 'ben', tags: ['x', 'y'] }, 2);"
    " var _px = _pp.createIndex('by_name', 'name');"
    " var _pm = _pp.createIndex('by_tag', 'tags', { multiEntry: true });"
    " var _pg = _px.get('ben'); _pg.onsuccess = function(){ _ix += ':pop' + _pg.result.name; };"
    " var _pc = _pm.count('x'); _pc.onsuccess = function(){ _ix += ':back' + _pc.result; };"
    " var _xc = _mx.count('b');"
    " _xc.onsuccess = function(){ fetch('/api/idbidx?v=' + _ix + ':multi' + _xc.result); }; };"
    "_r.onsuccess = function(){"
    " var _k2 = _r.result.transaction('kp').objectStore('kp');"
    " _idb += ':' + _k2.keyPath.join('+') + ':' + (_k2.keyPath === _k.keyPath ? 'shared' : 'perhandle');"
    " fetch('/api/idbopen?v=' + _idb + ':' + _r.result.version + ':' + _r.result.name);"
    /* §5.5 STEP 2 THROUGH §5.5 ITSELF. One record is added under a key the store has none for and one the
       store does hold is deleted, and the abort has to undo BOTH — which is also the pair whose inverses are
       a removal and a re-file, run backwards. The verification is a second transaction, because the aborted
       one is FINISHED and every member of §4.5 refuses one. */
    " var _at = _r.result.transaction('ord', 'readwrite'), _as = _at.objectStore('ord');"
    " _as.put(77, 9); _as.delete(1);"
    " var _m9 = _as.get(9), _m1 = _as.get(1);"
    /* THE ABORT RUNS FROM A HANDLER, NOT FROM THIS TASK. `abort()` called here would reach §5.5 step 6 before
       any of the four requests had executed — the operations would never run, there would be nothing to
       revert, and both `undone` terms would read TRUE for a transaction that had changed nothing. So the two
       writes are read back first, and `made77gone` is what makes the claim below about a real state. */
    " _m1.onsuccess = function(){"
    "  _rec += ':made' + _m9.result + (_m1.result === undefined ? 'gone' : 'LEFT'); _at.abort(); };"
    " _at.onabort = function(){"
    "  var _vs = _r.result.transaction('ord').objectStore('ord');"
    "  var _v9 = _vs.get(9), _v1 = _vs.get(1);"
    "  _v1.onsuccess = function(){"
    /* `h` IS THE CONTROL FOR `t`, ON THE SAME FETCH so that one endpoint record carries both. `t` is
       `location.hash` after a round trip through an object store; `h` is the SAME source reaching the SAME
       param list without one. Without it a `t` that does not read `{hash}` cannot say whether the STORE
       de-tainted the value or whether this source never reaches an @H param as a shape at all — two unrelated
       mechanisms under one 0, which is the defect the row above was just split to remove. */
    "   fetch('/api/idbrec?v=' + _rec + ':undone' + (_v9.result === undefined ? 'A' : 'LEAKA')"
    "        + (_v1.result === 10 ? 'D' : 'LEAKD') + '&t=' + _tv + '&h=' + location.hash); }; }; };"

    /* §4.5's WORKED EXAMPLE, IN ITS OWN DATABASE because its whole point is that the UPGRADE TRANSACTION dies:
       two records queued under one name, then a UNIQUE index over that name. The standard's own words are that
       "the index's uniqueness constraint does not cause the second request to fail. Instead, the transaction
       will be aborted when the index is created and the constraint fails." So `ConstraintError` is the
       TRANSACTION's error (§4.10's getter over what §5.10 step 9.3 aborted with) and NOT any request's — the
       second `put` succeeded — and the `AbortError` beside it is §5.1 step 10.8's, which is what a page's own
       `open` reports for an upgrade that aborted. Both halves are load-bearing: the first says the failure was
       the index creation's, and the second says the open FAILED rather than handing the page a connection to a
       half-migrated database. */
    "var _u = indexedDB.open('uniqfix', 1); var _uq = 'NOABORT';"
    "_u.onupgradeneeded = function(){"
    " var _us = _u.result.createObjectStore('u');"
    " _us.put({ name: 'betty' }, 1);"
    " _us.put({ name: 'betty' }, 2);"
    " _us.createIndex('by_name', 'name', { unique: true });"
    " var _ut = _u.transaction;"
    " _ut.onabort = function(){ _uq = _ut.error.name; }; };"
    "_u.onerror = function(){ fetch('/api/idbuniq?v=' + _uq + ':' + _u.error.name); };"
    "</script>"
    /* THE LAST <script> OF THE DOCUMENT, and it exists only to REPORT — see the injection in script 1. It has
       to be a document script rather than a task, because what is being proved is that a program injected from
       a NON-FINAL <script> took a position ahead of the document's remaining ones. */
    "<script>__so += 'C'; fetch('/api/scriptorder?seq=' + __so);</script>"
    /* HTML §3.1.7 "DOM tree accessors"'s `currentScript`, and the §4.12.1.1 "Processing model" bracket that
       writes it. THE FIXTURE PROVES THE DESIGN AND NOT THE MEMBER: a getter that reads a slot is one line, and
       what is hard here is that "run the classic script" is a WORK ITEM — it begins at one scheduler step and
       ends at another, with sibling flows running programs of their own in between — so a C save/restore
       bracket around the compile would answer one flow's element to another and would pass any fixture that
       runs a single script.
       `csB` FORKS ON UNKNOWN INPUT AND ONE ARM INJECTS A SCRIPT, which is what makes the two arms run
       DIFFERENT elements at the same cursor: the admin arm's `csINJ` runs at the slot §4.12.1.1's "immediately
       execute the script element" names, while the other arm is at `csC`. Under a slot that is not per-flow,
       whichever arm ran second reads the other's element and `/api/csend` reports `csINJ`.
       THE READ IS `getAttribute` AND THE FETCH IS BUILT FROM IT, because that is the real shape: nodejs.org's
       chunk preamble derives its asset prefix from `document.currentScript.getAttribute("src")`, so an
       undefined `currentScript` computes every lazy-chunk URL wrong — the moat surface, not a nicety. */
    "<script id=csA>window.__cs = document.currentScript.getAttribute('id') + ':' +"
    " (document.currentScript.getAttribute('src') === null ? 'inline' : 'src');</script>"
    "<script id=csB>"
    "if (state.admin) { var csi = document.createElement('script'); csi.setAttribute('id', 'csINJ');"
    " csi.textContent = \"fetch('/api/csinj?el=' + document.currentScript.getAttribute('id'));\";"
    " document.body.appendChild(csi); }"
    /* §3.1.7: "Returns null if the Document is not currently executing a script … (e.g., because the running
       script is an event handler, or a timeout)". This is step 4's restore observed from OUTSIDE the bracket:
       the timer callback is a task of this flow that runs after every program of the document has completed,
       so a slot that was never restored reports the last element that ran instead of null. */
    "setTimeout(function(){ fetch('/api/cstimer?el=' +"
    " (document.currentScript === null ? 'null' : document.currentScript.getAttribute('id'))); }, 0);"
    "</script>"
    /* §4.12.1.1's MODULE arm asserts `currentScript` is null and never sets it — the standard's own assertion,
       which is a DCHECK at the module compile and is observable here. `import.meta` is what a module reads
       instead, which is why the member is not merely unset but has to be NULL. */
    "<script type=module>fetch('/api/csmod?el=' +"
    " (document.currentScript === null ? 'null' : 'LEAK'));</script>"
    "<script id=csC>fetch('/api/csend?el=' + __cs + '|' +"
    " document.currentScript.getAttribute('id'));</script>"
    "</body></html>";

/* MINIMAL ASan fixture (APICLIENT_ASAN_MIN=1) — the memory-sensitive CLONE/COW/verify paths ONLY, with tiny
   loops and few branches so the full ASan smoke's ~3-4x fork-tree blowup is avoided. This is the per-change
   memory gate (seconds under native ASan): forEach-callback deep clone (CONT_ARRAY_ITER), array-element COW
   capture, the generator-body fork + its dedup-REPLACE re-fork, and the @S candidate re-fire flows. Correctness
   /breadth is the emcc full smoke's job (node engine/build.mjs cow); this is purely "does the clone/free path
   corrupt memory." Keep it SMALL — every added branch multiplies the flow count and the ASan wall-clock. */
static const char *HTML_MIN =
    "<!doctype html><html><body>"
    "<script>var cfg = { admin: state.admin };</script>"
    "<script>"
    "[1,2].forEach(function(e){ var w = cfg.admin ? 'feADMIN' : 'fePUBLIC'; fetch('/api/fefork?e=' + e + w); });"   /* forEach callback deep clone */
    "var owa = ['base']; if (cfg.admin) { owa[0] = 'owA'; } else { owa[0] = 'owB'; } fetch('/api/owfork?a=' + owa[0]);"   /* array-element COW */
    "delete globalThis.gdGone; globalThis.gdTag = cfg.admin ? 'gdADMIN' : 'gdPUBLIC';"   /* slot removal on the EXOTIC global — see the full document */
    " fetch('/api/gdel?v=' + globalThis.gdTag + (globalThis.gdGone === undefined ? 'gdSOLE' : 'gdLEAK'));"
    " delete globalThis.gdTag;"
    "eval(\"'\" + state.code + \"'\");"                    /* @S eval re-fire flow */
    "setInnerHTML('<div>' + state.html + '</div>');"       /* @S innerHTML re-fire flow */
    "setLocation(state.next);"                              /* @S location re-fire flow */
    "function* ggf(){ if (cfg.admin) { yield 'ggADMIN'; } else { yield 'ggPUBLIC'; } } var ggi = ggf(); fetch('/api/genfork?v=' + ggi.next().value);"   /* generator-body fork */
    "function* g2f(){ var a = cfg.admin ? 'A' : 'P'; var b = state.beta ? 'X' : 'Y'; yield 'g2:'+a+b; } var g2i = g2f(); fetch('/api/gen2fork?v=' + g2i.next().value);"   /* gen dedup-REPLACE re-fork */
    "function* gof(){ if (cfg.admin) { yield 'ofA'; } else { yield 'ofP'; } } var ofr=''; for (const x of gof()) { ofr += x; } fetch('/api/genofork?v=' + ofr);"   /* FOR-OF generator-body fork */
    "function* aff(){ if (cfg.admin) { yield 'afA'; } else { yield 'afP'; } } fetch('/api/afromfork?v=' + Array.from(aff())[0]);"   /* Array.from(GEN) consumer fork: the gen body branches while CONSUMED by Array.from on the tramp (CONT_ITER_CONSUME), so clone_deep_flow's gen-branch clones the JSIterConsume state -> both afA and afP */
    "function* spf(){ if (cfg.admin) { yield 'spA'; } else { yield 'spP'; } } fetch('/api/spreadfork?v=' + [...spf()][0]);"   /* [...GEN] spread consumer fork: same CONT_ITER_CONSUME machinery, SPREAD sink (append to the literal's array), forks mid-consume -> both spA and spP */
    /* OPAQUE ITERATION: `state.items` is unknown injected state, so LengthOfArrayLike over it has no answer -
       every length is a world, and the walk's bound is a CHAIN of per-position outcome forks
       (step_length_unknown). THIS STATEMENT IS RED, and each thing it has been red FOR is measured, because a
       row that names the wrong cause is what makes the next reader fix the wrong thing.
       FIXED: the constraint map's whole-map deep copy. Every fork copied the running flow's ENTIRE path
       constraint, so n positions cost O(n^2) bytes and the run died at the FIRST script in concolic.c's own
       CHECK. concolic.c is now a mutable head over refcounted immutable segments (cow.c's CowSeg pattern), so a
       fork costs O(1) - measured: the first script now completes and the walk runs.
       NOT the cause, though it was named as one: the per-position ask DOES return to the scheduler. The driver
       consults the preempt hook at every outcome fork (JS_PREEMPT_FORK, quickjs.c), so a yield is offered per
       position and the seam assertion's `asked == 0` never holds here.
       FIXED, and it was the reason this row read 227 seconds: the WALKING FLOW WAS NEVER OUTRANKED. flow_weight
       is reward + optimism - aging; the walker's reward is every endpoint it emitted EARLIER in this document
       while a fresh sibling starts at zero, and the aging that §scheduler says must sink "a monopolizer that
       burns CPU without emitting" was 1e-6 per SCHEDULER STEP - about 10^7 steps to give back ONE emission's
       worth of rank, and denominated in something the reward was not. So the scheduler was offered the yield,
       declined it, and re-picked the walker: 227 seconds of run with `switches` still at 1 and nothing else in
       the frontier ever dispatched. The charge is now MICROSECONDS OF THREAD TIME at the same 1e-6, i.e. one
       emitted finding per second held without emitting, so a flow that has emitted V findings and then stops
       falls below a never-run sibling after V + 1 seconds - and flow_best asserts that crossing rather than
       claiming it, so a rate that stops being commensurate crashes here instead of reading as a slow run. The
       227 seconds is the PRE-FIX measurement and is kept as the thing the unit is judged against; what the row
       costs now has not been re-measured, and the number to look at when it is is `switches`, which was 1.
       AND BEHIND THAT, the walk is unbounded by design (§NO BOUNDS: every length is a world and the walker
       takes the "longer" arm forever), so the frontier NEVER DRAINS. That was measured as this harness hanging,
       because its completion condition WAS that the frontier drains - a condition no document is obliged to
       meet. It is now the probe table (see it): the run ends when every statement this document makes has been
       answered, and the residue that is left over is real and is reported rather than waited on. What the tail
       needs after that is the mechanism engine.c already names at its own flow-compile OOM - a cold tier paging
       the lowest-value tail out of the live frontier - and on a NATIVE target nothing ever asks it to, because
       nothing refuses an allocation until the machine does (solver/reclaim.h owns that edge, and `sold` in
       @PROGRESS is what reports it). */
    "var _af = Array.from(state.items); fetch('/api/optiter?n=' + _af.length + '&a=' + _af[0] + '&b=' + _af[1]);"
    "var _sad = new Set(); if (cfg.admin) { _sad.add('sadA'); } else { _sad.add('sadP'); } fetch('/api/setaddfork?v=' + [...(_sad)][0]);"   /* SHARED-SET record isolation: _sad is created before the concolic fork; each arm's Set.add must be COW-isolated via the map_add capture (record removed by JS_MapDeleteRecord on unapply) -> EXACTLY 'sadA' and 'sadP', never a contaminated set holding both */
    "function* sef(){ if (cfg.admin) { yield 'seA'; } else { yield 'seP'; } } fetch('/api/setfork?v=' + [...new Set(sef())][0]);"   /* new Set(GEN) consumer fork: the Set consumer (CONT_ITER_CONSUME, SET sink) forks mid-consume; now fork-SAFE via the map_add COW capture -> both seA and seP */
    "var _mm = new Map([['k','base']]); if (cfg.admin) { _mm.set('k','mmA'); } else { _mm.delete('k'); } fetch('/api/mapmutfork?v=' + (_mm.has('k') ? _mm.get('k') : 'gone'));"   /* SHARED-MAP overwrite/delete isolation: _mm is created before the fork; one arm OVERWRITES 'k', the other DELETES it. The map_mutate undo-log capture (unapply restores the old value / re-adds) keeps them per-flow -> EXACTLY 'mmA' and 'gone', never cross-contaminated */
    "(async function(){ function* afsf(){ if (cfg.admin) { yield 'afsA'; } else { yield 'afsP'; } } var out=[]; for await (var x of afsf()) { out.push(x); } fetch('/api/afsfork?v=' + out[0]); })();"   /* for-await(GEN) consumer fork: the sync gen body branches while driven by the async-from-sync consumer (CONT_ASYNC_FROM_SYNC) on the tramp — clone_deep_flow clones the JSAsyncFromSync state with a FRESH wrapper promise per arm -> both afsA and afsP */
    "function* paf(){ if (cfg.admin) { yield Promise.resolve('pafA'); } else { yield Promise.resolve('pafP'); } } Promise.all(paf()).then(function(a){ fetch('/api/paffork?v=' + a[0]); });"   /* Promise.all(GEN) consumer fork at index==0: the gen branches during the FIRST .next() before any element .then is attached (CONT_PROMISE_ALL) — clone_deep_flow clones the JSPromiseAll aggregate fresh per arm -> both pafA and pafP */
    "function* paf2(){ yield Promise.resolve('p0'); if (cfg.admin) { yield Promise.resolve('pf2A'); } else { yield Promise.resolve('pf2P'); } } Promise.all(paf2()).then(function(a){ fetch('/api/paf2fork?v=' + a[0] + '-' + a[1]); });"   /* Promise.all(GEN) consumer fork at index>0: the gen yields element 0 THEN branches (fork during .next() #2, index==1). The retained pre-fork element wrapper (p0) is RE-ATTACHED to the sibling aggregate -> BOTH arms resolve a[0]=='p0' AND their own a[1] (p0-pf2A and p0-pf2P) */
    "var _rr = 'x1y2'.replace(/\\d/g, function(d){ return cfg.admin ? 'rrA'+d : 'rrP'+d; }); fetch('/api/rerepfork?v=' + _rr);"   /* @@replace CALLBACK fork (JSReRep): an OBJECT searchValue dispatches to RegExp.prototype[@@replace], whose machine holds THREE things across each callback — the collected match array, the spec's captures List in its own block, and the StringBuffer accumulator with nextSourcePosition. The replacer branches on opaque state at the FIRST of two matches, so the sibling must get its own copy of all three or the second match lands in the wrong accumulator. Pure paths xrrA1yrrA2 and xrrP1yrrP2 both present => each arm substituted BOTH matches into its OWN buffer. */
    "var _red=[1,2].reduce(function(acc,x){ return acc + (cfg.admin ? 'rA' : 'rP') + x; }, 'r:'); fetch('/api/redfork?v=' + _red);"   /* reduce ACCUMULATOR fork (CONT_ARRAY_REDUCE): the reducer branches on opaque state mid-fold, so clone_deep_flow clones the JSArrayReduce accumulator per arm. The pure paths r:rA1rA2 and r:rP1rP2 both present ⇒ each arm threaded its OWN accumulator across both elements (never a shared/contaminated acc) */
    "var _tpj={toString:function(){ return cfg.admin ? 'tpA' : 'tpP'; }}; fetch('/api/toprimfork?v=' + ['x','y'].join(_tpj));"   /* MACHINE-MODE ToPrimitive fork: 23.1.3.18 Array.prototype.join coerces its SEPARATOR, so §7.1.1 ToPrimitive runs the page's toString ON THE TRAMP with the JSArrayJoin machine as the JSToPrim's OUTER requester — a machine that owns no frame and is reachable only through `outer`. The branch is INSIDE that toString. Both pure paths xtpAy and xtpPy present ⇒ tramp_cont_relink_outer cloned the machine as well as the sequence, so each arm finished its own join with its own separator (a shared machine would leave one arm's separator in the other's buffer, or free the cursor twice) */
    "function* gcf(){ if (cfg.admin) { yield 'gcA'; } else { yield 'gcP'; } } var gci=gcf(); fetch('/api/gcallfork?v=' + gci.next.call(gci).value);"   /* generator .next() driven via .call BYPASS: gci.next.call(gci) reshapes at do_forward_call to the [this=gen, f=next] shape and is now routed onto do_generator_tramp (not the js_generator_next drive-to-completion). The body branches -> both gcA and gcP, never a DFAIL */
    "function* gapf(){ if (cfg.admin) { yield 'gapA'; } else { yield 'gapP'; } } var gapi=gapf(); fetch('/api/gapplyfork?v=' + gapi.next.apply(gapi, []).value);"   /* generator .next() via Function.prototype.apply BYPASS: reshaped at OP_call_method to [this=gen, next, arg0] and routed onto do_generator_tramp -> both gapA and gapP, never drive-to-completion */
    "function* graf(){ if (cfg.admin) { yield 'graA'; } else { yield 'graP'; } } var grai=graf(); fetch('/api/grefapplyfork?v=' + Reflect.apply(grai.next, grai, []).value);"   /* generator .next() via Reflect.apply BYPASS: [Reflect,apply,target,this,argsList] reshaped to [this=gen, next, arg0] and routed onto do_generator_tramp -> both graA and graP */
    /* A SYNCHRONOUS READ THE HOST MUST ANSWER — the shape `iframe.contentWindow.document.body` has. The value
       arrives at the CALL SITE, on the next line, with no promise anywhere: between the two the flow suspended,
       the scheduler ran other flows, the host answered, and this flow resumed. Inside a LOOP so it happens
       repeatedly, and the concatenation is what proves each answer reached the call that asked for it rather
       than an earlier or later one. */
    "var _hr=''; for (var _i=0;_i<3;_i++) { _hr += hostRead('hr'+_i); } fetch('/api/hostreq?v=' + _hr);"
    /* AND ONE ON EACH SIDE OF A FORK. A blocked flow that forks must RE-ISSUE its request under the sibling's
       own world — sharing the id would deliver one answer into two call sites in two contradictory worlds. */
    "fetch('/api/hostreqfork?v=' + hostRead(cfg.admin ? 'hrA' : 'hrP'));"
    /* §7.4: a child navigable. `open()` hands back a WindowProxy for a document in ANOTHER instance AT ITS OWN
       CALL SITE — the child's name is minted here, so there is nothing to ask and nothing to suspend for. */
    "var _w = open(); fetch('/api/navopen?v=' + (_w ? 'proxy' : 'null'));"
    /* §7.2.5.1's SAME-ORIGIN CHECK. A popup at ANOTHER origin exposes the fixed cross-origin list and nothing
       else: `closed` answers, `name` is a SecurityError. Both halves are the assertion — a filter that threw
       for everything would pass a test that only checked the throw. */
    "var _x = open('https://other.test/p'), _sop = '';"
    "try { _x.name; _sop += 'LEAKED'; } catch (e) { _sop += e.name; }"
    "fetch('/api/sop?v=' + _sop + ':' + (_x.closed === false ? 'closedok' : 'wrong'));"
    "</script>"
    "</body></html>";

/* THE CROSS-SESSION ROUND TRIP'S DOCUMENT — the third, and it exists because neither of the other two can be
 * parked. cold_park refuses a frontier that holds a segment of a FOREIGN world, naming the cross-instance park
 * that is not built; both documents above open child navigables (§7.4 `open()`, the cross-origin popup, the
 * iframe loads), so a whole-frontier park of either aborts there. That crash is CORRECT and stays exactly where
 * it is — what it means is that the residue this fixture can honestly write today is one from a document with
 * no peer in it, and that is what this document is.
 *
 * WHAT IT HAS TO CARRY IS THE RECIPE GRAMMAR — one statement per record kind — because a residue made of one
 * kind proves nothing about the arms that read the others, and until this document existed the only residue any
 * process in this tree had ever produced was a single `f-,<val>`: a park taken before the first pick, one
 * unstarted boot flow standing on nothing (engine/solvergate.mjs's `park` schedule). Every other arm of
 * cold_resume — the segment rebuild, the hex decode, the sink-class re-bind, the probe address — had never run.
 *   - A CONCOLIC BRANCH (`cfg.admin`), so flows stand on FROZEN DECISION SEGMENTS: the park then writes 's'
 *     records and the ordinals that name them, and the resume walks a chain instead of a stub.
 *   - A LOOP, so the flows are PREEMPTED mid-body on their back-edges and the recorded path is one that was
 *     suspended and rebuilt many times before it was written down.
 *   - AN @S SINK (`eval`), so solve.c seeds candidate sessions and the park writes 'c' records. They are the
 *     only records that carry ATTACKER TEXT, so they are the only path through park_hex/park_unhex, and their
 *     sink class is the only field that crosses the tier by NAME and has to be re-bound to a live pointer.
 *   - AN ENDPOINT, which is what makes this origin enter the endpoint surface at all.
 *
 * WHAT IT DOES NOT PRODUCE is one kind and no longer two. A 'd' record is an engine-seeded discovery probe and
 * active discovery is the trusted zone's, so that arm cannot run at all. 'f' and 'c' now arrive TOGETHER, which
 * is a correction of what stood here: candidates used to be seeded only where the frontier had drained, so a 'c'
 * record implied there was no exploration flow left to write an 'f' for. They are seeded at the PICK now (a
 * candidate is a work item the moment its sink exists — engine.c), so the moment this host parks at holds both
 * the arms still exploring and the candidates verifying, and the residue is 's' + 'f' + 'c'.
 * It is deliberately SMALL in every other respect: one fork and a six-iteration loop, so the document drains in
 * a second session rather than becoming a frontier this fixture cannot finish measuring. */
static const char *HTML_COLD =
    "<!doctype html><html><body>"
    "<script>var cfg = { admin: state.admin };</script>"
    "<script>"
    "var acc = 0; for (var i = 0; i < 6; i++) { acc += i; }"                       /* back-edges: a preempt mid-body, so a parked flow is FRAMED */
    "if (cfg.admin) { fetch('/api/cold/admin?a=' + acc); }"                        /* the fork: two arms, two decision chains, and an endpoint per arm */
    " else { fetch('/api/cold/public?a=' + acc); }"
    "eval(\"'\" + state.code + \"'\");"                                            /* the @S sink: candidate sessions, hence 'c' records */
    /* AND CODE THE PAGE SHIPPED AND NEVER CALLED, which is the one member of this frontier whose work a
       decision vector cannot reproduce — a resume re-runs the document, and re-running the document is exactly
       the run that never calls this. So it is what makes the park write an 'o' record and the resume rebuild a
       drive out of one, and without it every arm of that round trip was unreachable from any process in this
       tree: the three documents above had uncalled functions only in the EXPLORING one, which never parks.
       THREE THINGS IN ONE BODY, each of them a different half of the round trip.
         - A LOOP, so the drive is PREEMPTED on its back-edges and the parked drive is one with a live frame,
           rather than a flow that happened to be sitting at its entry.
         - A BRANCH ON ITS OWN ARGUMENT, which is unknown external input because nothing ever supplied one: the
           arms fork, so several flows carry the SAME locator and the resume has to hand one body back to all
           of them. Handing it to the first would starve the rest, and one arm is what a residue of one drive
           cannot catch.
         - A NESTED FUNCTION NOTHING CALLS EITHER, which is not an orphan yet — it has no closure until this
           body runs, which is what quickjs's take describes and what driving the enclosing one produces. It
           keeps the orphan set non-empty across the park moment below rather than leaving it a single body
           that is taken once and gone. */
    "function coldOrphan(q) {"
      "var s = 0; for (var j = 0; j < 4; j++) { s += j; }"
      "function coldOrphanInner(r) { fetch('/api/cold/orphan2?r=' + r + '&s=' + s); }"
      "if (q) { fetch('/api/cold/orphanA?s=' + s); } else { fetch('/api/cold/orphanB?s=' + s); }"
    "}"
    "</script>"
    "</body></html>";

/* HTML §7.2.6 AND CSP §6.1, in C — the browser half's tests are C tests, and this one has no page to run.
   What it pins is the pair of facts the rest of the platform will build on: what a policy PERMITS, and that a
   child's container is a CLONE whose answers do not move when the parent's would. */
/* TRUSTED TYPES §4.2.3 AND §3.8, in C, for the same reason the one below is: a parse over a serialized CSP
   list and a table lookup, neither of which has a page to run. What §4.2.3 pins is the answer every HTML
   sink's step 1 turns on — a document with no policy requires nothing, and one that requires trusted types is
   one on which `el.innerHTML = s` THROWS, so getting either side wrong silently changes what the solver
   explores on every page that carries a policy. What §3.8 pins is WHICH attributes are sinks at all: get that
   set too wide and every setAttribute on a policy-carrying page throws, too narrow and the solver reports
   `script.src = attackerUrl` as reachable on a document where the assignment dies. */
static void trusted_types_selftest(void)
{
    /* "No Content-Security-Policy" is the overwhelmingly common case, and a wrong default here would throw a
       TypeError out of every innerHTML assignment on the web. */
    CHECK(!trusted_types_required_by(NULL, TRUSTED_TYPE_HTML), "no policy must require no trusted type");
    CHECK(!trusted_types_required_by("script-src 'self'", TRUSTED_TYPE_HTML),
          "a policy without require-trusted-types-for must require no trusted type");
    CHECK(trusted_types_required_by("require-trusted-types-for 'script'", TRUSTED_TYPE_HTML),
          "require-trusted-types-for 'script' must require one at an HTML sink");
    /* CSP §2.2: directives are `;`-delimited within one policy, so the directive is found wherever it sits. */
    CHECK(trusted_types_required_by("script-src 'self'; require-trusted-types-for 'script'", TRUSTED_TYPE_SCRIPT),
          "a directive after another in the same policy must still be read");
    /* And POLICIES are `,`-delimited, enforced independently — so ANY of them requiring trusted types requires
       them, which is the opposite quantifier from policy_allows_inline and the one a copy of that loop would get
       wrong. */
    CHECK(trusted_types_required_by("default-src 'none', require-trusted-types-for 'script'",
                                    TRUSTED_TYPE_SCRIPT_URL),
          "a second policy in the list requiring trusted types must require them");
    /* The grammar writes a sink group as a QUOTED keyword. An unquoted token is not the keyword, and a
       directive with no value covers no group — the value IS the set it covers. */
    CHECK(!trusted_types_required_by("require-trusted-types-for script", TRUSTED_TYPE_HTML),
          "an unquoted sink group is not the keyword");
    CHECK(!trusted_types_required_by("require-trusted-types-for", TRUSTED_TYPE_HTML),
          "a directive with no value requires nothing");

    /* §3.8's four table rows and its event-handler rule. The sink NAME is checked as well as the type: it is
       what a violation report identifies, and two sinks reporting one name are one sink to whoever reads it. */
    {
        static const char *HTML_NS = "http://www.w3.org/1999/xhtml";
        static const char *SVG_NS = "http://www.w3.org/2000/svg";
        static const char *XLINK_NS = "http://www.w3.org/1999/xlink";
        TrustedTypeKind k;
        char sink[96];

        CHECK(trusted_types_attribute_data(HTML_NS, "iframe", NULL, "srcdoc", &k, sink, sizeof(sink)) &&
              k == TRUSTED_TYPE_HTML && strcmp(sink, "HTMLIFrameElement srcdoc") == 0,
              "an iframe's srcdoc is the TrustedHTML row");
        CHECK(trusted_types_attribute_data(HTML_NS, "script", NULL, "src", &k, sink, sizeof(sink)) &&
              k == TRUSTED_TYPE_SCRIPT_URL && strcmp(sink, "HTMLScriptElement src") == 0,
              "a script's src is the TrustedScriptURL row");
        CHECK(trusted_types_attribute_data(SVG_NS, "script", NULL, "href", &k, sink, sizeof(sink)) &&
              k == TRUSTED_TYPE_SCRIPT_URL, "an SVG script's href is the TrustedScriptURL row");
        CHECK(trusted_types_attribute_data(SVG_NS, "script", XLINK_NS, "href", &k, sink, sizeof(sink)) &&
              k == TRUSTED_TYPE_SCRIPT_URL, "an SVG script's xlink:href is its own TrustedScriptURL row");
        /* The INTERFACE is what the first column names, and an `iframe`'s src is not its srcdoc — a row read
           as "any element, this attribute" would make every `img.src` a Trusted Types sink. */
        CHECK(!trusted_types_attribute_data(HTML_NS, "iframe", NULL, "src", &k, sink, sizeof(sink)),
              "an iframe's src is not in the table");
        CHECK(!trusted_types_attribute_data(HTML_NS, "img", NULL, "src", &k, sink, sizeof(sink)),
              "an img's src is not the script row");
        CHECK(!trusted_types_attribute_data(SVG_NS, "script", NULL, "src", &k, sink, sizeof(sink)),
              "an SVG script's src is not its href row");
        /* STEP 2, which precedes the table and builds its sink name out of the attribute. */
        CHECK(trusted_types_attribute_data(HTML_NS, "div", NULL, "onclick", &k, sink, sizeof(sink)) &&
              k == TRUSTED_TYPE_SCRIPT && strcmp(sink, "Element onclick") == 0,
              "an event handler content attribute is a TrustedScript sink named after itself");
        CHECK(trusted_types_attribute_data(SVG_NS, "circle", NULL, "onload", &k, sink, sizeof(sink)) &&
              k == TRUSTED_TYPE_SCRIPT, "step 2 covers the SVG namespace as well as HTML");
        /* An attribute NAMESPACE is what step 2 requires to be null, and `ongoing` is not a handler name — a
           prefix match would make it one. */
        CHECK(!trusted_types_attribute_data(HTML_NS, "div", XLINK_NS, "onclick", &k, sink, sizeof(sink)),
              "a namespaced onclick is not an event handler content attribute");
        CHECK(!trusted_types_attribute_data(HTML_NS, "div", NULL, "ongoing", &k, sink, sizeof(sink)),
              "a name that merely starts like a handler is not one");
        CHECK(!trusted_types_attribute_data(HTML_NS, "div", NULL, "onclickx", &k, sink, sizeof(sink)),
              "a name that merely begins with a handler name is not one");
        CHECK(!trusted_types_attribute_data(HTML_NS, "div", NULL, "title", &k, sink, sizeof(sink)),
              "an ordinary attribute maps to nothing, which is nearly every attribute there is");
    }
}

/* CSP §6.7.2 — URL MATCHING, ONE SOURCE EXPRESSION AT A TIME.
 *
 * IT IS ASSERTED AT THE EXPRESSION rather than only through a container, because §6.7.2.8's five arms and the
 * four relations under them are where every real policy's answer is decided, and a container test can only
 * ever exercise one arm per policy it builds. The container half is asserted below it, over §4.1.2, which is
 * the composition and nothing more.
 *
 * EVERY PAIR OF LINES THAT MUST DISAGREE IS WRITTEN AS A PAIR. A matcher that answered "Matches" for
 * everything passes a suite that only checks the matches — the same shape of self-agreeing test §Security
 * names for the cross-origin filter — so each relation is stated in both directions. */
static void csp_url_matching_selftest(void)
{
    /* THE SELF-ORIGIN OF EVERY LIST BELOW, and the reason the fixture states TWO: `'self'` and a schemeless
       host-source are the two arms whose answer depends on it, and an http origin is what makes §6.7.2.9's
       secure upgrade observable. */
    const Origin *https_self = origin_parse("https://x.test");
    const Origin *http_self = origin_parse("http://x.test");
    /* An OPAQUE self-origin — §7.1.1's "no serialization it can be recreated from" — which §2.2's own note
       says the field exists to carry. It matches nothing, and that must be a computed answer rather than a
       crash on reading components it does not have. */
    const Origin *opaque_self = origin_parse("null");
    struct { const char *expr, *url; const Origin *self; CspMatch want; const char *why; } CASES[] = {
        /* §6.7.2.8 step 1 — `*` is HTTP(S), or the self-origin's own scheme, and nothing else. */
        { "*", "https://any.example/x", https_self, CSP_MATCHES, "`*` matches any HTTP(S) URL" },
        { "*", "data:text/html,x", https_self, CSP_DOES_NOT_MATCH,
          "`*` is not a licence for `data:` — that is what the note about explicit schemes says" },
        /* §6.7.2.9 through a scheme-source, in BOTH directions: the relation is asymmetric. */
        { "http:", "https://a.test/x", https_self, CSP_MATCHES, "http: upgrades to https" },
        { "https:", "http://a.test/x", https_self, CSP_DOES_NOT_MATCH, "https: does NOT downgrade to http" },
        { "ws:", "https://a.test/x", https_self, CSP_MATCHES, "§6.7.2.9 maps ws: onto http and https too" },
        { "wss:", "http://a.test/x", https_self, CSP_DOES_NOT_MATCH, "wss: reaches https alone" },
        /* §6.7.2.10 — the wildcard label, the exact host, and the asymmetry between them. */
        { "*.example.com", "https://www.example.com/x", https_self, CSP_MATCHES, "*. matches a subdomain" },
        { "*.example.com", "https://example.com/x", https_self, CSP_DOES_NOT_MATCH,
          "the dot in `remaining` is what stops *.example.com matching the bare domain" },
        { "*.example.com", "https://notexample.com/x", https_self, CSP_DOES_NOT_MATCH,
          "a suffix test WITHOUT the dot would match this, which is the bug the dot exists to prevent" },
        { "www.example.com", "https://www.example.com/x", https_self, CSP_MATCHES, "an exact host matches" },
        { "WWW.Example.COM", "https://www.example.com/x", https_self, CSP_MATCHES,
          "§6.7.2.10 step 4 is an ASCII case-insensitive match" },
        { "www.example.com", "https://sub.www.example.com/x", https_self, CSP_DOES_NOT_MATCH,
          "host-part matching is asymmetric — a host does not match a pattern naming its parent" },
        /* §2.3.1's note, made structural by §6.7.2.10 step 1: an IP address is not a domain. */
        { "127.0.0.1", "https://127.0.0.1/x", https_self, CSP_DOES_NOT_MATCH,
          "`127.0.0.1` parses to an IPv4 NUMBER, which is not a domain, so no host-part can name it" },
        /* §6.7.2.11 — null against null, an explicit port, a scheme default, and `*`. */
        { "a.test", "http://a.test/x", http_self, CSP_MATCHES,
          "a null port-part matches a URL whose port the parser dropped" },
        { "a.test", "http://a.test:8080/x", http_self, CSP_DOES_NOT_MATCH,
          "and refuses one that carries a port, which is the whole of what a portless expression means" },
        { "http://a.test:8080", "http://a.test:8080/x", http_self, CSP_MATCHES, "an explicit port matches" },
        { "https://a.test:443", "https://a.test/x", https_self, CSP_MATCHES,
          "§6.7.2.11 step 5: a null URL port IS the scheme's default port" },
        { "http://a.test:*", "http://a.test:9/x", http_self, CSP_MATCHES, "`*` is any port" },
        /* §6.7.2.12 — the trailing solidus decides prefix versus exact, and both directions are asserted. */
        { "https://a.test/a/", "https://a.test/a/b.js", https_self, CSP_MATCHES, "a trailing / is a PREFIX" },
        { "https://a.test/a/b.js", "https://a.test/a/b.js", https_self, CSP_MATCHES, "an exact path matches" },
        { "https://a.test/a/b.js", "https://a.test/a/b.js/c", https_self, CSP_DOES_NOT_MATCH,
          "no trailing solidus means EXACT, so a longer path is not a match" },
        { "https://a.test/a/", "https://a.test/b/c", https_self, CSP_DOES_NOT_MATCH, "a different prefix" },
        /* Step 8 compares PERCENT-DECODED segments, and the two sides must be spelled DIFFERENTLY for that to
           be what is under test: `~` is outside §4.4's path percent-encode set so the URL parser leaves it
           literal, while the expression writes it encoded. A byte comparison answers "Does Not Match" here. */
        { "https://a.test/a%7Eb", "https://a.test/a~b", https_self, CSP_MATCHES,
          "§6.7.2.12 step 8 compares percent-DECODED segments, so `%7E` and `~` are one path segment" },
        /* THE PATH IS NOT COMPARED ON A REDIRECTED REQUEST — asserted through the same expression, so the two
           lines differ in the redirect count alone. That case is the loop below's second pass. */
        /* §6.7.2.8 step 4 — `'self'`, both bullets and the opaque case. */
        { "'self'", "https://x.test/y", https_self, CSP_MATCHES, "'self' is the self-origin outright" },
        { "'self'", "https://other.test/y", https_self, CSP_DOES_NOT_MATCH, "and nothing else" },
        { "'self'", "https://x.test/y", http_self, CSP_MATCHES,
          "the second bullet: an http origin's 'self' UPGRADES to https on the same host at default ports" },
        { "'self'", "http://x.test/y", https_self, CSP_DOES_NOT_MATCH,
          "and never downgrades — an https origin's 'self' does not reach http" },
        { "'self'", "https://x.test/y", opaque_self, CSP_DOES_NOT_MATCH,
          "an OPAQUE self-origin is same origin with nothing and has no host to compare" },
        { "a.test", "https://a.test/x", opaque_self, CSP_DOES_NOT_MATCH,
          "and a SCHEMELESS host-source has no origin scheme to be measured against either" },
        /* §6.7.2.8 step 5 — every keyword, nonce and hash names no URL. */
        { "'unsafe-inline'", "https://x.test/y", https_self, CSP_DOES_NOT_MATCH,
          "'unsafe-inline' permits no LOAD at all, which is the answer that surprises policy authors" },
        { "'nonce-abc'", "https://x.test/y", https_self, CSP_DOES_NOT_MATCH, "nor does a nonce" },
        /* Expressions the grammar refuses, which must fall to step 5 rather than be read loosely. */
        { "*.", "https://x.test/y", https_self, CSP_DOES_NOT_MATCH,
          "`*.` has no label after it, so §2.3.1's `1*host-char` refuses it" },
        { "under_score.test", "https://under_score.test/y", https_self, CSP_DOES_NOT_MATCH,
          "host-char admits no underscore — §2.3.1 requires Punycode for anything outside it" },
    };
    size_t i;

    for (i = 0; i < sizeof CASES / sizeof *CASES; i++) {
        UrlRecord u;
        CspToken e;

        url_record_init(&u);
        CHECK(url_parse(&u, CASES[i].url, strlen(CASES[i].url), NULL),
              "the CSP matching fixture named a URL its own parser refuses");
        e.p = CASES[i].expr;
        e.n = strlen(CASES[i].expr);
        CHECK(csp_source_match_url(e, &u, CASES[i].self, 0) == CASES[i].want, CASES[i].why);
        url_record_free(&u);
    }

    /* §6.7.2.8's REDIRECT-COUNT CLAUSE, stated as one expression answered two ways: the path is compared on a
       fresh request and DROPPED once a redirect has happened, because the path a redirect landed on is the
       server's choice and not the policy author's. Two calls that differ in one argument and must disagree. */
    {
        UrlRecord u;
        CspToken e;

        url_record_init(&u);
        CHECK(url_parse(&u, "https://a.test/other", 20, NULL), "the redirect fixture's URL would not parse");
        e.p = "https://a.test/a/";
        e.n = 17;
        CHECK(csp_source_match_url(e, &u, https_self, 0) == CSP_DOES_NOT_MATCH,
              "an unredirected request is measured against the expression's path");
        CHECK(csp_source_match_url(e, &u, https_self, 1) == CSP_MATCHES,
              "and a redirected one is not — §6.7.2.8 drops the path comparison once redirect count is not 0");
        url_record_free(&u);
    }

    /* §6.7.2.7's THREE PRE-LOOP STEPS, over a real directive so the `'none'` rules are read off the parse. */
    {
        static const struct { const char *policy; const char *url; CspRequestVerdict want; const char *why; }
        LISTS[] = {
            { "connect-src", "https://x.test/a", CSP_REQUEST_BLOCKED,
              "§6.7.2.7 step 2: an EMPTY source list matches no URL — `connect-src` with no value is 'none'" },
            { "connect-src 'none'", "https://x.test/a", CSP_REQUEST_BLOCKED, "§6.7.2.7 step 3: 'none' alone" },
            { "connect-src 'none' https://x.test", "https://x.test/a", CSP_REQUEST_ALLOWED,
              "§6.7.2.7's second note: 'none' has NO effect when another expression is present" },
            /* §6.8.3/§6.8.4 through §4.1.2: default-src governs a fetch, and a present connect-src REPLACES
               it rather than adding to it. The two lines differ by the second directive alone. */
            { "default-src 'self'", "https://x.test/a", CSP_REQUEST_ALLOWED,
              "§6.8.3: connect-src falls back to default-src, and 'self' is this document's origin" },
            { "default-src 'self'", "https://other.test/a", CSP_REQUEST_BLOCKED,
              "and a cross-origin URL is not 'self'" },
            { "default-src https://other.test; connect-src 'self'", "https://other.test/a",
              CSP_REQUEST_BLOCKED,
              "§6.8.4: a present connect-src REPLACES default-src for a fetch rather than inheriting it" },
            /* A policy that governs nothing about this request says nothing about it. */
            { "img-src 'none'; frame-ancestors 'none'", "https://other.test/a", CSP_REQUEST_ALLOWED,
              "no directive of this policy is in connect-src's fallback list, so §6.8.4 answers No for all" },
            /* §2.2: two POLICIES intersect, so the second can only narrow. */
            { "connect-src *, connect-src 'self'", "https://other.test/a", CSP_REQUEST_BLOCKED,
              "policies in a list are enforced independently — the second refuses what the first permits" },
        };
        size_t k;

        for (k = 0; k < sizeof LISTS / sizeof *LISTS; k++) {
            PolicyContainer *p = policy_container_new(LISTS[k].policy, https_self, NULL);
            UrlRecord u;

            url_record_init(&u);
            CHECK(url_parse(&u, LISTS[k].url, strlen(LISTS[k].url), NULL),
                  "the §4.1.2 fixture named a URL its own parser refuses");
            CHECK(policy_should_block_request(p, &u, "", 0) == LISTS[k].want, LISTS[k].why);
            url_record_free(&u);
            policy_container_free(p);
        }
    }

    /* A DOCUMENT WITH NO POLICY BLOCKS NOTHING, which is the overwhelmingly common page and the one a wrong
       default would refuse every request on. */
    {
        PolicyContainer *none = policy_container_new(NULL, https_self, NULL);
        UrlRecord u;

        url_record_init(&u);
        CHECK(url_parse(&u, "https://anywhere.test/a", 23, NULL), "the no-policy fixture's URL would not parse");
        CHECK(policy_should_block_request(none, &u, "", 0) == CSP_REQUEST_ALLOWED,
              "a document with no Content-Security-Policy must permit every request");
        /* And §6.8.1's "report" destination is governed by no fetch directive even under `default-src 'none'`,
           which is what stops a report to a blocked endpoint from being blocked and never sent. */
        policy_container_free(none);
        none = policy_container_new("default-src 'none'", https_self, NULL);
        CHECK(policy_should_block_request(none, &u, "report", 0) == CSP_REQUEST_ALLOWED,
              "§6.8.1 returns null for the report destination, so no fetch directive governs it");
        CHECK(policy_should_block_request(none, &u, "", 0) == CSP_REQUEST_BLOCKED,
              "while the same policy blocks an ordinary fetch — the two lines differ in the destination alone");
        url_record_free(&u);
        policy_container_free(none);
    }
}

/* §4.2.3 asked with NO ELEMENT and an EMPTY SOURCE — the shape §4.2.4 uses when it runs the inline check
   "upon null" for a javascript: navigation, and the shape the solver asks about markup it has not inserted
   anywhere. Every assertion below that turns on a TYPE and a source LIST goes through it, so the few that
   carry a real element stay visibly different from the many that cannot. */
static bool csp_ok(const PolicyContainer *p, CspInlineType type)
{
    return policy_allows_inline(p, type, NULL, "", 0);
}

static void policy_container_selftest(void)
{
    PolicyContainer *none, *self_only, *inline_ok, *nonced, *evals, *child;
    /* CSP §2.2's SELF-ORIGIN for every list below. It is minted from a serialization rather than taken from
       origin_agent(), because these algorithms are answerable with no agent at all — which is the point of
       running them before the runtime exists. */
    const Origin *self_origin = origin_parse("https://x.test");

    none = policy_container_new(NULL, self_origin, NULL);
    /* No policy is not an empty policy: a document with no Content-Security-Policy permits everything, which
       is the overwhelmingly common case and the one a wrong default would mis-report on every page. */
    CHECK(csp_ok(none, CSP_INLINE_SCRIPT_ATTRIBUTE), "no policy must permit an inline handler");
    CHECK(policy_allows_string_compilation(none), "no policy must permit eval");

    self_only = policy_container_new("script-src 'self'", self_origin, NULL);
    /* §S's own example: an inline onerror is DEAD under `script-src 'self'`, and so is a javascript: URL. A
       host source never permits inline execution — that is what 'unsafe-inline' is for. */
    CHECK(!csp_ok(self_only, CSP_INLINE_SCRIPT_ATTRIBUTE), "'self' must not permit an inline handler");
    CHECK(!csp_ok(self_only, CSP_INLINE_NAVIGATION), "'self' must not permit a javascript: URL");
    CHECK(!policy_allows_string_compilation(self_only), "'self' must not permit eval");

    inline_ok = policy_container_new("default-src 'none'; script-src 'unsafe-inline'", self_origin, NULL);
    CHECK(csp_ok(inline_ok, CSP_INLINE_SCRIPT_ATTRIBUTE), "'unsafe-inline' must permit an inline handler");
    CHECK(!policy_allows_string_compilation(inline_ok), "'unsafe-inline' must not permit eval");

    /* CSP §6.1: a nonce source makes 'unsafe-inline' be IGNORED — the rule that makes adding a nonce to a
       legacy policy actually tighten it rather than widen it. A handler can carry no nonce, so it stays dead. */
    nonced = policy_container_new("script-src 'unsafe-inline' 'nonce-abc'", self_origin, NULL);
    CHECK(!csp_ok(nonced, CSP_INLINE_SCRIPT), "a nonce source must make 'unsafe-inline' ignored");
    CHECK(!csp_ok(nonced, CSP_INLINE_SCRIPT_ATTRIBUTE), "a handler carries no nonce, so it stays blocked");

    /* CSP §6.1: a present `script-src` REPLACES `default-src` for scripts rather than adding to it. The first
       version of this parser OR'd every script-governing directive's sources into one flag set, and this is
       the case that exposes it — the handler came out ALLOWED because `default-src`'s 'unsafe-inline' survived
       a `script-src` that does not carry it. */
    {
        PolicyContainer *overridden = policy_container_new("default-src 'unsafe-inline'; script-src 'self'", self_origin, NULL);
        CHECK(!csp_ok(overridden, CSP_INLINE_SCRIPT_ATTRIBUTE),
              "a present script-src must REPLACE default-src for scripts, not inherit its 'unsafe-inline'");
        policy_container_free(overridden);
    }
    /* §6.8.2 + §6.8.4's granular forms, chosen per KIND: script-src-attr governs event handlers, script-src-
       elem governs script elements AND javascript: navigations, and eval reads script-src past both.
       The javascript: line is the one this suite had BACKWARDS. It asserted that script-src-attr governs a
       javascript: URL too, which is what policy_container.c's own enum comment claimed and what its directive
       choice implemented — so this policy reported a javascript: URL as blocked where a real browser runs it.
       §6.8.2 maps the inline type "navigation" to script-src-elem, and §6.1.11 says the same from the other
       side. The two lines below now differ, which is the whole content of the fix. */
    {
        PolicyContainer *granular =
            policy_container_new("script-src 'unsafe-inline' 'unsafe-eval'; script-src-attr 'none'", self_origin, NULL);
        CHECK(!csp_ok(granular, CSP_INLINE_SCRIPT_ATTRIBUTE), "script-src-attr 'none' must kill a handler");
        CHECK(csp_ok(granular, CSP_INLINE_NAVIGATION),
              "§6.8.2 maps a `navigation` inline check to script-src-elem, so script-src-attr must not touch it");
        CHECK(csp_ok(granular, CSP_INLINE_SCRIPT),
              "script-src-attr must not govern a script ELEMENT — that is script-src-elem's fallback to script-src");
        CHECK(policy_allows_string_compilation(granular), "eval has no granular form and reads script-src");
        policy_container_free(granular);
    }
    /* ...and the same policy with the granular form that DOES govern a navigation kills it, which is what
       makes the line above a statement about which directive rather than about which answer. */
    {
        PolicyContainer *elem =
            policy_container_new("script-src 'unsafe-inline'; script-src-elem 'none'", self_origin, NULL);
        CHECK(!csp_ok(elem, CSP_INLINE_NAVIGATION), "script-src-elem 'none' must kill a javascript: URL");
        CHECK(!csp_ok(elem, CSP_INLINE_SCRIPT), "script-src-elem 'none' must kill a script element");
        CHECK(csp_ok(elem, CSP_INLINE_SCRIPT_ATTRIBUTE), "script-src-elem must not govern an event handler");
        policy_container_free(elem);
    }
    /* §6.7.3.2's OTHER two overrides of 'unsafe-inline', neither of which this file could answer before: a
       HASH source and 'strict-dynamic' both make the whole directive stop allowing all inline behavior. Every
       one of these policies used to ABORT the process — the old parser recorded any source expression it did
       not model and then DCHECKed on it, which is most of the real web. */
    {
        PolicyContainer *hashed =
            policy_container_new("script-src 'unsafe-inline' 'sha256-YWJj'", self_origin, NULL);
        /* The SCRIPT-ATTRIBUTE type is the one this can assert, and the reason is §6.7.3.3's own step 5: its
           hash arm runs only when the type is "script" or "style" or the list carries 'unsafe-hashes', so a
           handler never reaches a digest and the answer is decided by §6.7.3.2 alone.
           THE `CSP_INLINE_SCRIPT` LINE THAT STOOD BESIDE THIS ONE IS DELETED RATHER THAN KEPT AS A PASSING
           ASSERTION, because it was right by accident: it asserted "Blocked" while the engine computed that
           from the hash merely being PRESENT, and the true answer is "Blocked unless sha256(source) is YWJj".
           §6.7.3.3 now reaches for the digest and DFAILs, which is the honest state — see
           csp_source_list.c's step 5.2.2. */
        CHECK(!csp_ok(hashed, CSP_INLINE_SCRIPT_ATTRIBUTE),
              "a hash source overrides 'unsafe-inline' for the WHOLE directive, handlers included");
        policy_container_free(hashed);
    }
    {
        /* `'sha1-…'` is not a hash-source: §2.3.1's hash-algorithm is exactly sha256/sha384/sha512, so this is
           an unrecognised expression, which §6.7.3.2 IGNORES rather than treats as an override. The two lines
           differ by the digest length alone and must not agree. */
        PolicyContainer *not_a_hash = policy_container_new("script-src 'unsafe-inline' 'sha1-YWJj'", self_origin, NULL);
        CHECK(csp_ok(not_a_hash, CSP_INLINE_SCRIPT),
              "an expression outside the grammar must be ignored by §6.7.3.2, not read as a hash source");
        policy_container_free(not_a_hash);
    }
    {
        PolicyContainer *strict = policy_container_new("script-src 'unsafe-inline' 'strict-dynamic'", self_origin, NULL);
        CHECK(!csp_ok(strict, CSP_INLINE_SCRIPT), "'strict-dynamic' must override 'unsafe-inline'");
        CHECK(!csp_ok(strict, CSP_INLINE_NAVIGATION),
              "'strict-dynamic' covers the navigation type as well as script and script attribute");
        policy_container_free(strict);
    }
    /* ...AND THE SAME EXPRESSION OVER A STYLE DIRECTIVE MUST NOT OVERRIDE ANYTHING. §6.7.3.2's second test
       names three types — "script", "script attribute", "navigation" — and its own note says 'strict-dynamic'
       does not apply to other resource types. These two lines differ from the two above by the DIRECTIVE and
       the TYPE alone and must not agree; a copy of the script arm that forgot the type would silently refuse
       every page that pairs a style directive with 'strict-dynamic', which strict CSPs routinely do. */
    {
        PolicyContainer *sd = policy_container_new("style-src 'unsafe-inline' 'strict-dynamic'", self_origin, NULL);
        CHECK(csp_ok(sd, CSP_INLINE_STYLE), "'strict-dynamic' must not touch inline style");
        CHECK(csp_ok(sd, CSP_INLINE_STYLE_ATTRIBUTE), "nor a style attribute");
        policy_container_free(sd);
    }
    /* THE STYLE HALF OF §6.8.2 AND §6.8.3, which is the whole reason core/html/html_style_element.c can now
       run §4.2.6 step 5 at all. `style-src-elem` governs the ELEMENT, `style-src-attr` the ATTRIBUTE, both
       fall back through `style-src` and then `default-src`, and no script directive reaches either. */
    {
        PolicyContainer *d = policy_container_new("default-src 'unsafe-inline'", self_origin, NULL);
        PolicyContainer *g = policy_container_new("default-src 'unsafe-inline'; style-src-attr 'none'",
                                                 self_origin, NULL);
        PolicyContainer *s = policy_container_new("default-src 'unsafe-inline'; style-src 'none'",
                                                 self_origin, NULL);
        PolicyContainer *k = policy_container_new("script-src 'none'; style-src 'unsafe-inline'",
                                                 self_origin, NULL);

        CHECK(csp_ok(d, CSP_INLINE_STYLE) && csp_ok(d, CSP_INLINE_STYLE_ATTRIBUTE),
              "§6.8.3 falls back to default-src for both style types when no style directive is present");
        CHECK(csp_ok(g, CSP_INLINE_STYLE), "style-src-attr must not govern a style ELEMENT");
        CHECK(!csp_ok(g, CSP_INLINE_STYLE_ATTRIBUTE), "…which is exactly what it does govern");
        CHECK(!csp_ok(s, CSP_INLINE_STYLE) && !csp_ok(s, CSP_INLINE_STYLE_ATTRIBUTE),
              "a present style-src REPLACES default-src for both style types rather than adding to it");
        CHECK(csp_ok(k, CSP_INLINE_STYLE), "a script directive must not reach a style check");
        CHECK(!csp_ok(k, CSP_INLINE_SCRIPT), "…and the same policy still kills the script it names");
        policy_container_free(d);
        policy_container_free(g);
        policy_container_free(s);
        policy_container_free(k);
    }
    /* HOST AND SCHEME SOURCES ARE NOT AN INLINE ANSWER AT ALL — §6.7.3.2 never looks at them — so a policy
       made of them permits exactly what its keywords permit. This is the shape almost every real policy has,
       and it is the one the old parser aborted on. */
    {
        PolicyContainer *hosts =
            policy_container_new("script-src https: https://*.example.com:443/a/b 'unsafe-inline'", self_origin, NULL);
        CHECK(csp_ok(hosts, CSP_INLINE_SCRIPT_ATTRIBUTE),
              "host and scheme sources are invisible to §6.7.3.2, so 'unsafe-inline' still allows all inline");
        CHECK(!policy_allows_string_compilation(hosts), "and none of them is 'unsafe-eval'");
        policy_container_free(hosts);
    }
    /* §2.2.1: within ONE policy a repeated directive is IGNORED, so the first wins... */
    {
        PolicyContainer *dup = policy_container_new("script-src 'unsafe-inline'; script-src 'self'", self_origin, NULL);
        CHECK(csp_ok(dup, CSP_INLINE_SCRIPT_ATTRIBUTE), "a repeated directive in one policy must be ignored");
        policy_container_free(dup);
    }
    /* ...and §2.2.1 lowercases the name before that containment test, so the repeat is a repeat however it is
       spelled — `script-SRC 'none'` and `ScRiPt-sRc 'none'` are the standard's own example of equivalence. */
    {
        PolicyContainer *cased = policy_container_new("SCRIPT-SRC 'unsafe-inline'; script-src 'none'", self_origin, NULL);
        CHECK(csp_ok(cased, CSP_INLINE_SCRIPT_ATTRIBUTE),
              "a directive name is matched ASCII case-insensitively, so the second one is the ignored repeat");
        policy_container_free(cased);
    }
    /* ...but §2.2's policy LIST is comma-delimited and enforced INDEPENDENTLY, so the very same two directives
       as two POLICIES must intersect instead. These two lines differ by one character and must not agree. */
    {
        PolicyContainer *list = policy_container_new("script-src 'unsafe-inline', script-src 'self'", self_origin, NULL);
        CHECK(!csp_ok(list, CSP_INLINE_SCRIPT_ATTRIBUTE),
              "policies in a list are enforced independently — a second policy can only NARROW");
        policy_container_free(list);
    }
    /* A policy that governs no script directive forbids nothing about scripts. */
    {
        PolicyContainer *unrelated = policy_container_new("img-src 'none'; frame-ancestors 'none'", self_origin, NULL);
        CHECK(csp_ok(unrelated, CSP_INLINE_SCRIPT_ATTRIBUTE), "img-src must not block a handler");
        CHECK(policy_allows_string_compilation(unrelated), "frame-ancestors must not block eval");
        policy_container_free(unrelated);
    }

    evals = policy_container_new("script-src 'unsafe-eval'", self_origin, NULL);
    CHECK(policy_allows_string_compilation(evals), "'unsafe-eval' must permit eval");
    CHECK(!csp_ok(evals, CSP_INLINE_SCRIPT_ATTRIBUTE), "'unsafe-eval' must not permit an inline handler");

    /* §7.4: the initial about:blank's container is a CLONE of its creator's — which is the whole of how a
       same-origin popup or iframe with no URL inherits CSP, and it is a deep copy so the child's answers do
       not move when the parent is navigated. */
    child = policy_container_clone(self_only);
    CHECK(!csp_ok(child, CSP_INLINE_SCRIPT_ATTRIBUTE), "a cloned container must carry the creator's policy");
    CHECK(policy_container_csp(child) != NULL &&
          policy_container_csp(child) != policy_container_csp(self_only),
          "a cloned container must own its own copy of the policy text");

    policy_container_free(none);
    policy_container_free(self_only);
    policy_container_free(inline_ok);
    policy_container_free(nonced);
    policy_container_free(evals);
    policy_container_free(child);
}

/* §6.7.3.1 "Is element nonceable?" AND §6.7.3.3's NONCE ARM — the half of the inline check that cannot be
 * asked without an element, and the half that decides whether the page's OWN content runs.
 *
 * IT IS THE CASE THAT TURNS AN ABORT INTO A WRONG ANSWER IF IT IS GOT WRONG. §6.7.3.2 alone answers "Blocked"
 * for every nonce-bearing policy, which is right for injected content and refuses a `<style nonce=…>` real
 * Chrome applies — so the whole cascade below it would then resolve from a document no browser has. */
/* FIPS PUB 180-4, CHECKED AGAINST PUBLISHED KNOWN ANSWERS — because a cryptographic primitive that agrees with
 * itself proves nothing at all. Every expected value below is transcribed from RFC 6234 §8.5's test driver,
 * which is the IETF's restatement of this standard and ships the vectors with it; the empty-message row is the
 * one this fixture adds, and its oracle is an independent implementation cross-checked against those same
 * eight published vectors before it was asked for the ninth.
 *
 * THE MESSAGES ARE CHOSEN FOR THE BLOCK BOUNDARY AND NOT FOR VARIETY. "abc" fits one block with room for the
 * padding. RFC 6234's TEST2_1 is 56 bytes, which for a 512-bit block leaves NO room for the 64-bit length —
 * so SHA-1 and SHA-256 must pad into a SECOND block, which is the case an implementation gets wrong. TEST2_2
 * is 112 bytes and does the same to the 1024-bit block of SHA-384 and SHA-512. The empty message exercises the
 * path where the padding is the entire message. */
static void secure_hash_selftest(void)
{
    static const char T1[]  = "abc";
    static const char T21[] = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    static const char T22[] = "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmn"
                              "hijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu";
    static const struct { SecureHashAlgorithm alg; const char *msg; size_t len; const char *hex; } KAT[] = {
        /* RFC 6234 §8.5, SHA1 tests 1 and 2 */
        { SECURE_HASH_SHA1,   T1,  sizeof T1 - 1,  "A9993E364706816ABA3E25717850C26C9CD0D89D" },
        { SECURE_HASH_SHA1,   T21, sizeof T21 - 1, "84983E441C3BD26EBAAE4AA1F95129E5E54670F1" },
        /* RFC 6234 §8.5, SHA256 tests 1 and 2 */
        { SECURE_HASH_SHA256, T1,  sizeof T1 - 1,
          "BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD" },
        { SECURE_HASH_SHA256, T21, sizeof T21 - 1,
          "248D6A61D20638B8E5C026930C3E6039A33CE45964FF2167F6ECEDD419DB06C1" },
        /* RFC 6234 §8.5, SHA384 tests 1 and 2 */
        { SECURE_HASH_SHA384, T1,  sizeof T1 - 1,
          "CB00753F45A35E8BB5A03D699AC65007272C32AB0EDED163"
          "1A8B605A43FF5BED8086072BA1E7CC2358BAECA134C825A7" },
        { SECURE_HASH_SHA384, T22, sizeof T22 - 1,
          "09330C33F71147E83D192FC782CD1B4753111B173B3B05D2"
          "2FA08086E3B0F712FCC7C71A557E2DB966C3E9FA91746039" },
        /* RFC 6234 §8.5, SHA512 tests 1 and 2 */
        { SECURE_HASH_SHA512, T1,  sizeof T1 - 1,
          "DDAF35A193617ABACC417349AE20413112E6FA4E89A97EA2"
          "0A9EEEE64B55D39A2192992A274FC1A836BA3C23A3FEEBBD"
          "454D4423643CE80E2A9AC94FA54CA49F" },
        { SECURE_HASH_SHA512, T22, sizeof T22 - 1,
          "8E959B75DAE313DA8CF4F72814FC143F8F7779C6EB9F7FA1"
          "7299AEADB6889018501D289E4900F7E4331B99DEC4B5433A"
          "C7D329EEB6DD26545E96E55B874BE909" },
        /* The empty message: §5.1's padding is the whole of what is hashed. */
        { SECURE_HASH_SHA1,   "", 0, "DA39A3EE5E6B4B0D3255BFEF95601890AFD80709" },
        { SECURE_HASH_SHA256, "", 0,
          "E3B0C44298FC1C149AFBF4C8996FB92427AE41E4649B934CA495991B7852B855" },
        { SECURE_HASH_SHA384, "", 0,
          "38B060A751AC96384CD9327EB1B1E36A21FDB71114BE0743"
          "4C0CC7BF63F6E1DA274EDEBFE76F65FBD51AD2F14898B95B" },
        { SECURE_HASH_SHA512, "", 0,
          "CF83E1357EEFB8BDF1542850D66D8007D620E4050B5715DC"
          "83F4A921D36CE9CE47D0D13C5D85F2B0FF8318D2877EEC2F"
          "63B931BD47417A81A538327AF927DA3E" },
    };
    static const char HEX[] = "0123456789ABCDEF";
    size_t k;

    for (k = 0; k < sizeof KAT / sizeof KAT[0]; k++) {
        uint8_t out[SECURE_HASH_MAX_DIGEST];
        char got[2 * SECURE_HASH_MAX_DIGEST + 1];
        size_t n = secure_hash_digest_size(KAT[k].alg), i;
        SecureHash h;

        secure_hash_init(&h, KAT[k].alg);
        secure_hash_update(&h, (const uint8_t *)KAT[k].msg, KAT[k].len);
        secure_hash_finish(&h, out, sizeof out);
        for (i = 0; i < n; i++) { got[2 * i] = HEX[out[i] >> 4]; got[2 * i + 1] = HEX[out[i] & 15]; }
        got[2 * n] = 0;
        CHECK(strlen(KAT[k].hex) == 2 * n,
              "a known-answer row's digest is not the length FIPS 180-4 Figure 1 gives its algorithm — the row "
              "and the table disagree, and one of them is a transcription error");
        CHECK(strcmp(got, KAT[k].hex) == 0,
              "FIPS 180-4 answered a PUBLISHED known-answer vector wrongly — the port is wrong, and every "
              "digest this engine has ever produced is with it");
    }

    /* THE STREAMING SPLIT MUST NOT CHANGE THE ANSWER, which is the property the digest member depends on: it
       feeds ONE message block per turn and yields between them, so a partial-block buffer that mishandled a
       boundary would produce a different digest for the same message depending on how the scheduler sliced it.
       Every split of the 56-byte vector is taken, which is every state the buffer can be left in. */
    {
        size_t cut;

        for (cut = 0; cut <= sizeof T21 - 1; cut++) {
            uint8_t out[SECURE_HASH_MAX_DIGEST];
            char got[2 * SECURE_HASH_MAX_DIGEST + 1];
            size_t i;
            SecureHash h;

            secure_hash_init(&h, SECURE_HASH_SHA256);
            secure_hash_update(&h, (const uint8_t *)T21, cut);
            secure_hash_update(&h, (const uint8_t *)T21 + cut, (sizeof T21 - 1) - cut);
            secure_hash_finish(&h, out, sizeof out);
            for (i = 0; i < 32; i++) { got[2 * i] = HEX[out[i] >> 4]; got[2 * i + 1] = HEX[out[i] & 15]; }
            got[64] = 0;
            CHECK(strcmp(got, "248D6A61D20638B8E5C026930C3E6039A33CE45964FF2167F6ECEDD419DB06C1") == 0,
                  "a message split across two updates hashed differently from the same message in one — the "
                  "digest a page gets would then depend on where the scheduler happened to preempt the walk");
        }
    }
}

static void csp_element_matching_selftest(void)
{
    static const char *SRC =
        "<html><head>"
        "<style nonce=abc>p{color:red}</style>"
        "<style>p{color:blue}</style>"
        "</head><body></body></html>";
    lxb_html_document_t *dom = dom_document_create();
    lxb_dom_element_t *nonced = NULL, *bare = NULL;
    lxb_dom_node_t *child;
    const Origin *self_origin = origin_parse("https://x.test");
    const char *S = "p{color:red}";
    size_t slen = strlen(S);

    html_parse_document(dom, (const lxb_char_t *)SRC, strlen(SRC));
    /* The two `<style>` elements are the head's only element children, in source order and with no whitespace
       text between them — which is why the fixture is written on one line. */
    for (child = lxb_dom_interface_node(lxb_html_document_head_element(dom))->first_child; child;
         child = child->next) {
        if (child->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        if (!nonced) nonced = lxb_dom_interface_element(child);
        else if (!bare) bare = lxb_dom_interface_element(child);
    }
    CHECK(nonced != NULL && bare != NULL, "the fixture's two <style> elements were not both parsed into head");

    /* §6.7.3.1 steps 1 and 4: presence of a `nonce` attribute is the whole question for a non-script element. */
    CHECK(csp_element_is_nonceable(nonced), "a <style nonce=abc> is Nonceable");
    CHECK(!csp_element_is_nonceable(bare), "a <style> with no nonce attribute is Not Nonceable");
    CHECK(!csp_element_is_nonceable(NULL),
          "§4.2.4 runs the inline check upon NULL, and step 1 answers it without a branch of its own");

    {
        PolicyContainer *pol = policy_container_new("style-src 'nonce-abc'", self_origin, NULL);
        PolicyContainer *other = policy_container_new("style-src 'nonce-xyz'", self_origin, NULL);
        PolicyContainer *cased = policy_container_new("style-src 'NONCE-abc'", self_origin, NULL);
        PolicyContainer *upper = policy_container_new("style-src 'nonce-ABC'", self_origin, NULL);

        /* THE POINT OF THE WHOLE CHANGE: the nonced element runs and the identical element beside it does
           not, under one policy, decided by the element rather than by the policy alone. */
        CHECK(policy_allows_inline(pol, CSP_INLINE_STYLE, nonced, S, slen),
              "<style nonce=abc> under style-src 'nonce-abc' must be ALLOWED — §6.7.3.3's nonce arm");
        CHECK(!policy_allows_inline(pol, CSP_INLINE_STYLE, bare, S, slen),
              "…and the <style> beside it, with no nonce, must be Blocked by the same policy");
        CHECK(!policy_allows_inline(pol, CSP_INLINE_STYLE, NULL, S, slen),
              "…and so must an injected one, which has no element at all");
        CHECK(!policy_allows_inline(other, CSP_INLINE_STYLE, nonced, S, slen),
              "a nonce that does not match must not admit the element");
        /* §2.3.1's ABNF literal `'nonce-` is CASE-INSENSITIVE (RFC 5234 §2.3) and its base64-value is NOT:
           the value is data, not a keyword. These two lines differ only in which half is capitalised. */
        CHECK(policy_allows_inline(cased, CSP_INLINE_STYLE, nonced, S, slen),
              "the nonce-source PREFIX is matched ASCII case-insensitively");
        CHECK(!policy_allows_inline(upper, CSP_INLINE_STYLE, nonced, S, slen),
              "…while the base64-value is compared exactly, so 'nonce-ABC' must not admit a nonce of abc");
        /* §6.7.3.3's own note: "Nonces only apply to inline script and inline style, not to attributes of
           either element or to javascript: navigations." The element is the same one. */
        CHECK(!policy_allows_inline(pol, CSP_INLINE_STYLE_ATTRIBUTE, nonced, S, slen),
              "a nonce on the element must not admit a STYLE ATTRIBUTE — §6.7.3.3 step 2 excludes the type");
        policy_container_free(pol);
        policy_container_free(other);
        policy_container_free(cased);
        policy_container_free(upper);
    }

    /* §6.7.3.3 STEP 5.2.2 — THE HASH ARM, which used to be a DFAIL naming the digest to build. The expected
       values are the base64 of FIPS 180-4's digest over the same twelve bytes the fixture's first <style>
       carries, produced by an implementation cross-checked against RFC 6234's published vectors — the same
       oracle secure_hash_selftest above uses, and for the same reason.
       WHAT MAKES THIS THE TEST AND NOT A ROUND TRIP is the second half of each pair: ONE BYTE of the published
       hash is changed, and the identical element must then be Blocked. A hash arm that answered "Matches" for
       everything, or that compared lengths and nothing else, passes the first line of every pair. */
    {
        static const char S256[] = "style-src 'sha256-p0bF+un5yUb9MBO6xRb8kPHlY2BdpHVtLiFkDrZPF64='";
        static const char S384[] = "style-src 'sha384-fmjM3tAll81TWJvySQni8SaBymKJ5GlctvLrjS0TwyC8mMAW"
                                                    "6RDB6buj8mJRR2Dg'";
        static const char S512[] = "style-src 'sha512-ADKwpJY0f2osTsQ2rmUDaXZKZ3/ULiwyJYXVFAAUxw8Oh4Bq"
                                                    "MFlllZdCVwhLKbNk4+NFO7HzMJ3X7I4K69XhLQ=='";
        /* THE SAME SHA-512 HASH SPELLED IN BASE64URL — `/` written `_` and `+` written `-`, which is what
           §2.3.1's base64-value grammar admits and what step 5.2.2's replacement normalises. The two policies
           must give the SAME verdict, and the vector was picked because its base64 contains both characters:
           a hash with neither would pass this line with no replacement implemented at all. */
        static const char S512URL[] = "style-src 'sha512-ADKwpJY0f2osTsQ2rmUDaXZKZ3_ULiwyJYXVFAAUxw8Oh4Bq"
                                                       "MFlllZdCVwhLKbNk4-NFO7HzMJ3X7I4K69XhLQ=='";
        /* The last character of the base64-value changed, and nothing else. */
        static const char S256BAD[] = "style-src 'sha256-p0bF+un5yUb9MBO6xRb8kPHlY2BdpHVtLiFkDrZPF65='";
        /* §2.3.1's ABNF literals are case-insensitive (RFC 5234 §2.3), which for a hash-source is the
           ALGORITHM half; step 5.2.2 says so again in its own words ("an ASCII case-insensitive match for
           "sha256""). The base64-value stays data. */
        static const char S256CASE[] = "style-src 'SHA256-p0bF+un5yUb9MBO6xRb8kPHlY2BdpHVtLiFkDrZPF64='";
        /* §2.3.1 admits sha256/sha384/sha512 and NOTHING ELSE, so this expression is not a hash-source at all:
           §6.7.3.2 therefore ignores it, `'unsafe-inline'` is NOT overridden, and the element runs. It is the
           one case where the grammar's exactness is observable from a policy. */
        static const char S1[] = "style-src 'unsafe-inline' 'sha1-b4xQTHDAiKMmuZc8XlQ3hGJcGh0='";
        static const struct { const char *policy; int expect; const char *why; } ROWS[] = {
            { S256,    1, "a <style> whose SHA-256 the policy publishes must RUN — the whole point of a hash "
                          "source, and the answer 'Does Not Match' would have blocked the page's own block" },
            { S384,    1, "the same, with §6.7.3.3 step 5.2.2's SHA-384 arm" },
            { S512,    1, "the same, with its SHA-512 arm" },
            { S512URL, 1, "a base64url-spelled hash must match the same block — step 5.2.2 replaces '-' with "
                          "'+' and '_' with '/' before comparing" },
            { S256CASE,1, "the hash-algorithm part is matched ASCII case-insensitively" },
            { S256BAD, 0, "one byte of the published hash changed must BLOCK the identical element" },
            { S1,      1, "'sha1-…' is not a hash-source in §2.3.1's grammar, so §6.7.3.2 ignores it and the "
                          "'unsafe-inline' beside it is NOT overridden" },
        };
        size_t k;

        for (k = 0; k < sizeof ROWS / sizeof ROWS[0]; k++) {
            PolicyContainer *p = policy_container_new(ROWS[k].policy, self_origin, NULL);

            CHECK(!!policy_allows_inline(p, CSP_INLINE_STYLE, bare, S, slen) == ROWS[k].expect, ROWS[k].why);
            /* The ELEMENT is not what a hash arm reads — §6.7.3.3 step 5 is over `source` alone — so the
               nonce-less and the nonced element must answer alike under a hash policy. */
            CHECK(!!policy_allows_inline(p, CSP_INLINE_STYLE, nonced, S, slen) == ROWS[k].expect,
                  "§6.7.3.3's hash arm reads the SOURCE and not the element, so a nonce on the element must "
                  "not change its answer");
            policy_container_free(p);
        }

        /* §6.7.3.3 STEPS 3-4's 'unsafe-hashes' FLAG, which is the whole of what extends the hash arm past
           "script" and "style". Without it a STYLE ATTRIBUTE's source never reaches step 5 at all. */
        {
            PolicyContainer *plain = policy_container_new(S256, self_origin, NULL);
            PolicyContainer *hashes = policy_container_new(
                "style-src 'unsafe-hashes' 'sha256-p0bF+un5yUb9MBO6xRb8kPHlY2BdpHVtLiFkDrZPF64='",
                self_origin, NULL);

            CHECK(!policy_allows_inline(plain, CSP_INLINE_STYLE_ATTRIBUTE, bare, S, slen),
                  "a hash alone must NOT admit a style attribute — §6.7.3.3 step 5's condition is `type is "
                  "\"script\" or \"style\", or unsafe-hashes flag is true`");
            CHECK(policy_allows_inline(hashes, CSP_INLINE_STYLE_ATTRIBUTE, bare, S, slen),
                  "…and 'unsafe-hashes' beside it must, which is the only thing that flag does");
            policy_container_free(plain);
            policy_container_free(hashes);
        }
    }
    dom_document_destroy(dom);
}

/* WHAT MAKES THE CONTAINER LOAD-BEARING, which the parser test above does not reach: the parse can be perfect
   and still answer for nothing if neither of the document's two policy sources gets to it — the response's
   header and its own `<meta>`. Its own tree, because that is what makes it exercisable at all. */
static void document_policy_selftest(void)
{
    static const char *SRC =
        "<html><head>"
        "<meta charset=utf-8>"
        "<meta http-equiv='Content-Security-Policy' content=\"script-src 'unsafe-inline'\">"
        "<meta http-equiv='refresh' content='0'>"
        /* CASE-INSENSITIVE, which is how a real page spells it, and a SECOND policy — CSP composes by
           INTERSECTION, so a later policy can only narrow. Taking the last one instead would have reported
           this page as permitting eval. */
        "<meta HTTP-EQUIV='content-security-policy' content=\"script-src 'self'\">"
        "</head><body></body></html>";
    lxb_html_document_t *dom = dom_document_create();
    PolicyContainer *p, *empty;
    lxb_html_document_t *plain;
    const Origin *self_origin = origin_parse("https://x.test");

    html_parse_document(dom, (const lxb_char_t *)SRC, strlen(SRC));
    p = document_policy_new(dom, NULL, self_origin);
    CHECK(policy_container_csp(p) != NULL, "the meta scan found no policy in a document that declares two");
    CHECK(!csp_ok(p, CSP_INLINE_SCRIPT_ATTRIBUTE),
          "two meta policies must INTERSECT — the second's 'self' forbids what the first's 'unsafe-inline' "
          "permits, and a scan that let the last one win would report a live handler on a page that blocks it");
    CHECK(!policy_allows_string_compilation(p), "neither meta policy carries 'unsafe-eval'");

    /* A page with no CSP at all is the overwhelmingly common one, and the scan must not invent a policy for
       it — an empty container that answered "blocked" would suppress every real finding on every such page. */
    plain = dom_document_create();
    html_parse_document(plain, (const lxb_char_t *)"<html><body></body></html>", 26);
    empty = document_policy_new(plain, NULL, self_origin);
    CHECK(csp_ok(empty, CSP_INLINE_SCRIPT_ATTRIBUTE), "a document with no meta CSP must permit everything");

    {
        /* §7.2.6's OTHER HALF. The response header and the `<meta>` policies are ONE LIST and every policy in
           it is enforced, so a header that forbids inline must still forbid it on a page whose meta permits it
           — which is exactly the page above. The engine's entry point used to drop the header, so this page
           reported a live inline handler that the real response kills. */
        PolicyContainer *hdr = document_policy_new(plain, "script-src 'self'", self_origin);
        PolicyContainer *both = document_policy_new(dom, "default-src 'unsafe-inline'", self_origin);
        CHECK(!csp_ok(hdr, CSP_INLINE_SCRIPT_ATTRIBUTE),
              "a header-borne policy must be enforced on a document whose tree declares none");
        CHECK(!csp_ok(both, CSP_INLINE_SCRIPT_ATTRIBUTE),
              "the header and the meta policies are ONE LIST — a permissive header cannot widen a narrow meta");
        policy_container_free(hdr);
        policy_container_free(both);
    }

    policy_container_free(p);
    policy_container_free(empty);
    dom_document_destroy(dom);
    dom_document_destroy(plain);
}

/* THE CROSS-INSTANCE HALF OF THE COW DELTA. One WASM instance is one ORIGIN-KEYED AGENT CLUSTER, so a flow
   that scripts a CROSS-ORIGIN iframe or popup owns segments in two instances (a same-origin child is a second
   realm in this same heap and needs none of this); the delta cannot travel — it names its targets by live heap
   pointers — so the world's NAME travels and the peer builds its own segment.
   This exercises the peer's half against worlds minted as if by another document — the sender's half is
   exercised by every fork the fixture below performs, since flow_add mints and engine_fork_finalize forks. */
static void world_registry_selftest(JSContext *ctx)
{
    /* THE PEER'S DOCUMENT, INTERNED THROUGH THE ONE DOOR THAT MINTS A HANDLE. A `WorldId.doc` is this
       instance's INDEX into its own name table (solver/world.h), never a number a caller picks: every real
       arrival reaches world_segment through world_parse, which interns the NAME the wire form carries. This
       fixture wrote `{ 7, 1 }` — a handle into a table holding exactly one document — and it survived only
       because nothing ever asked the handle what it named. The park asks: a segment now keeps the VECTOR it
       was materialized from, and writing one resolves every handle in it. So this was not a fixture that
       exercised a rare state, it was a fixture MODELLING A DOCUMENT THAT DOES NOT EXIST, and the assert that
       caught it is the one that makes the wire form mean anything. */
    const uint32_t peer = world_doc_intern("peer");
    WorldId root = { peer, 1, 0 }, parent = { peer, 2, 0 }, child = { peer, 3, 0 }, stranger = { peer, 99, 0 };
    WorldId anc_child[2], anc_parent[1];
    CowDelta *d_root, *d_parent, *d_child, *d_stranger;

    anc_child[0] = parent; anc_child[1] = root;   /* NEAREST FIRST, as world_ancestry writes it */
    anc_parent[0] = root;

    /* A world that has never written in this document starts from its BASELINE. That is the truth rather than
       a default: an empty delta over the baseline is exactly what "wrote nothing here" means. */
    CHECK(!world_has_segment(root), "a world that never reached this instance must have no segment");
    d_root = world_segment(ctx, root, NULL, 0);
    CHECK(d_root != NULL && world_has_segment(root), "a segment must be materialized on first use");
    /* MATERIALIZED ONCE. A second lookup that built a second delta would give one world two timelines here,
       and the writes captured into the first would vanish at the next context switch. */
    CHECK(world_segment(ctx, root, NULL, 0) == d_root, "a world's segment must be materialized exactly once");

    d_parent = world_segment(ctx, parent, anc_parent, 1);
    CHECK(d_parent != d_root, "a child world must get its OWN segment, forked from its ancestor's");

    /* THE RULE THAT FAILS SILENTLY. With BOTH the parent and the grandparent present, forking the grandparent
       loses the parent's writes — and nothing reports it: the flow simply reads an older document than the one
       it wrote. So the scan must stop at the NEAREST ancestor, which is why world_ancestry's nearest-first
       ordering is asserted where it is produced. */
    d_child = world_segment(ctx, child, anc_child, 2);
    CHECK(d_child != d_parent && d_child != d_root, "a materialized world must not alias an ancestor's segment");

    /* An ancestry naming only worlds this instance has never seen is not an error — that flow wrote nothing
       here, so it starts from the baseline like any other. */
    d_stranger = world_segment(ctx, stranger, anc_parent, 1);
    CHECK(d_stranger != NULL, "an unknown ancestry must still yield a baseline segment");

    /* THE PARK'S HALF, AND THIS IS THE ONLY PLACE IN THE SMOKE IT CAN RUN — a foreign segment needs a peer,
     * and this fixture is one instance. What a park writes for a segment is the VECTOR it was materialized
     * from (solver/world.h), so two things have to be true and neither is visible from the record count:
     *   - THE VECTOR NAMES THE WORLD BACK. world_parse is world_serialize's inverse and the park's rebuild is
     *     exactly `world_parse` then `world_segment`, so a vector that round-trips to a different world
     *     rebuilds a segment under a name no sender uses.
     *   - THE ORDER IS MATERIALIZATION ORDER. A segment forks the nearest ancestor PRESENT when it is built,
     *     so an ancestor is always earlier, and the resumed session rebuilds in one forward pass. */
    {
        WorldId want[4]; const char *const *v; const WorldId *banc; WorldId back; int n, i;

        want[0] = root; want[1] = parent; want[2] = child; want[3] = stranger;
        n = world_segments_park(&v);
        CHECK(n == 4, "the park must carry every foreign segment this instance holds — one dropped here is a "
                      "peer document's timeline lost at the tier that exists to save it");
        for (i = 0; i < n; i++) {
            world_parse(v[i], &back, &banc);
            CHECK(world_eq(back, want[i]),
                  "a parked segment's vector must name the world it was materialized for, in materialization "
                  "order — the rebuild is world_parse then world_segment, so a vector that names something "
                  "else rebuilds a segment under a name no sender will ever use");
        }
        /* AND REMOVING A MIDDLE ONE MUST NOT MOVE A DESCENDANT ABOVE ITS OWN ANCESTOR. A swap-remove from the
           tail would put `stranger` where `parent` was, and the resumed session's forward pass would then meet
           `child` before `root` and rebuild it from the baseline — losing the ancestor's writes silently, the
           same loss a truncated vector causes, arriving from the other side. */
        world_release(ctx, parent);
        n = world_segments_park(&v);
        CHECK(n == 3, "releasing one segment must remove exactly one");
        world_parse(v[0], &back, &banc);
        CHECK(world_eq(back, root), "the segment table's order is materialization order and a release keeps it");
        world_parse(v[1], &back, &banc);
        CHECK(world_eq(back, child),
              "a released segment was swapped out from the tail rather than removed in place, so a descendant "
              "now stands above its own ancestor and a resumed session would rebuild it from the baseline");
    }

    world_release(ctx, child);
    CHECK(!world_has_segment(child), "a released world must no longer hold a segment");
    /* Releasing a world that never wrote here is a no-op: the sender cannot know which peers a flow reached,
       and tracking that only to avoid a no-op is state kept for nothing. */
    world_release(ctx, child);
    world_release(ctx, root);
    world_release(ctx, parent);
    world_release(ctx, stranger);
    CHECK(!world_has_segment(root) && !world_has_segment(parent), "every segment must be released");
}


/* ===== THE FLOW'S JOB QUEUE — HTML §8.1.7 "Event loops"' two queues, held as one JS Array =====
 *
 * A Flow's queue was the last malloc'd platform queue in this engine. What this exercises is the four things
 * that were hand-maintained around the struct it used to be, each of which is now one call:
 *   - the PICK is §8.1.7's microtask checkpoint and not a FIFO pop — a task may not begin while the flow still
 *     holds a microtask, which is the one ordering the event loop exists to forbid;
 *   - a FORK gives the arm its OWN Array naming the parent's RECORDS, so consuming or appending on one arm
 *     leaves the other exactly where it was, and the inheritance costs a refcount rather than a malloc per job
 *     plus a dup per argument;
 *   - §7.5.10 "Destroying documents"'s destroy a document step 7 removes every task of a destroyed document
 *     WITHOUT running it, keyed on the enqueuing REALM, and the survivors keep their arrival order;
 *   - the PROVENANCE bracket says which jobs a replay would not re-cause, which is what cold_park_flow reads.
 *
 * A ZEROED Flow ON THE STACK rather than a member of the frontier, deliberately: these five functions touch
 * exactly one field, so the fixture is the narrow interface CLAUDE.md asks a component to be exercised through
 * — a registry flow would drag a world, a delta and a release path into a test about a queue. */
static int g_tf_job_seen[8], g_tf_job_n;

static JSValue tf_job_note(JSContext *ctx, int argc, JSValueConst *argv)
{
    int32_t n = -1;

    CHECK(argc == 1, "the job queue handed a callee an argument count its record did not carry");
    JS_ToInt32(ctx, &n, argv[0]);
    CHECK(g_tf_job_n < (int)(sizeof g_tf_job_seen / sizeof *g_tf_job_seen), "too many jobs ran");
    g_tf_job_seen[g_tf_job_n++] = n;
    return JS_UNDEFINED;
}

/* A SECOND CALLEE, so the table hands out two ordinals and a record naming one cannot be run by the other. */
static JSValue tf_job_count(JSContext *ctx, int argc, JSValueConst *argv)
{
    (void)argv;
    return JS_NewInt32(ctx, 1000 + argc);
}

/* THE FIXTURE MINTS THE NAMES, because these pushes do not come through quickjs's enqueue and so no runtime
   counter has issued one for them. Monotone and never reused, which is the only property a handle has: this
   file is the whole population of pushers here, so one counter is exactly the runtime's guarantee. */
static JSTaskHandle g_tf_handle;

static JSTaskHandle tf_handle_new(void)
{
    return ++g_tf_handle;
}

static void tf_job_push_int(JSContext *ctx, Flow *f, int n, int task)
{
    JSValue v = JS_NewInt32(ctx, n);

    flow_job_push(ctx, f, tf_job_note, 1, (JSValueConst *)&v, task, tf_handle_new());
    JS_FreeValue(ctx, v);
}

static void tf_job_run_all(JSContext *ctx, Flow *f)
{
    while (flow_job_pending(f)) {
        JSValue e = flow_job_take(ctx, f);
        JSValue r = flow_job_run(ctx, e);

        JS_FreeValue(ctx, r);
        JS_FreeValue(ctx, e);
    }
}

/* ===== A SORT THE SCHEDULER CAN BE ASKED INSIDE, and the same answer under both schedules =====
 *
 * 23.1.3.30 Array.prototype.sort ( comparator ) with NO comparator is the arm that reaches no page code at
 * all: 23.1.3.30.2 CompareArrayElements steps 5-6 are ToString over numbers, which resolve in place, so the
 * whole of 23.1.3.30.1 SortIndexedProperties step 4 — the implementation-defined sequence of calls, O(n log n)
 * of them over an array the page grew — ran inside ONE def->step() and no schedule could get in. Running no
 * user code is not what makes a C body safe to leave un-parkable; being O(1) is.
 *
 * THE TWO SCHEDULES ARE THE ASSERTION, which is the differential §Testing describes reduced to one machine:
 * the same program, once with the policy declining every offer (nothing parks) and once with it taking every
 * one (every element placement parks the flow and rebuilds it), must produce the SAME completion value. A
 * resume that dropped, duplicated or reordered ONE placement lands on a different number.
 * AND THAT A PARK HAPPENED INSIDE THE MACHINE, which the answer alone cannot say — a sort driven to completion
 * also answers correctly. The runtime's step census is what says it: a flow suspended inside a
 * continuation-holding builtin leaves that builtin's state allocated, so a non-zero JS_StepMachineCount at a
 * suspension of a program whose ONLY builtin is this sort is the merge having rested mid-walk. */
static int tf_diff_preempt_always(int kind) { (void)kind; return 1; }
static int tf_diff_preempt_never(int kind)  { (void)kind; return 0; }
static const JSFlowControlHooks TF_DIFF_FORCE   = { .preempt = tf_diff_preempt_always };
static const JSFlowControlHooks TF_DIFF_DECLINE = { .preempt = tf_diff_preempt_never };
/* what this fixture puts back: no policy at all, which is what the runtime held before it ran. A hook that
   merely answers "no" is not the same state — it is a policy, and the next thing to install one would be
   replacing a decision rather than making the first. */
static const JSFlowControlHooks TF_DIFF_NONE    = { 0 };

/* The values are a permutation of 0..n-1 (337 is prime, so it is coprime with 300), so every element has a
   DISTINCT decimal string and the order 23.1.3.30.2 steps 5-10 put them in is unique — the answer does not
   rest on stability, and n = 300 is not a power of two, so the last block of a width pass is the ragged lone
   run whose drain has no comparison in it at all. The program checks its own answer rather than being compared
   against a constant: strict sortedness is decided by page code that never touches the merge, and the
   POSITIONAL checksum pins the exact permutation. -1 is "the sort's answer was wrong", which is a different
   failure from "the two schedules disagree" and must not be able to look like one. */
static const char *TF_SORT_SRC =
    "(function(){ var n = 300, a = [], i, h = 0, ok = 1;"
    " for (i = 0; i < n; i++) a[i] = (i * 337) % n;"
    " a.sort();"
    " if (a.length !== n) ok = 0;"
    " for (i = 0; i < n; i++) {"
    "   if (i > 0 && !(String(a[i - 1]) < String(a[i]))) ok = 0;"
    "   h = (h * 31 + a[i]) % 1000003; }"
    " return ok ? h : -1; })()";

/* Run a self-checking program to completion under `hooks`, reporting how many times it suspended and the
   largest step census seen at a suspension. Parameterised by the SOURCE because the differential is one
   procedure — the same program under two schedules — and a second copy of it per machine under test is the
   pair of walks that drifts. */
static int32_t tf_step_diff_run(JSContext *ctx, const char *src, const char *name,
                                const JSFlowControlHooks *hooks, int *psusp, int *pmach)
{
    JSRuntime *rt = JS_GetRuntime(ctx);
    JSValue *flow;
    JSValue res = JS_UNDEFINED;
    int32_t got = -2;
    int susp = 0, mach = 0, r;

    JS_SetFlowControlHooks(hooks);
    flow = JS_FlowNew(ctx, src, strlen(src), name, 0);
    CHECK(flow != NULL, "a step differential fixture could not create a flow");
    for (;;) {
        r = JS_FlowResume(ctx, flow, &res);
        CHECK(r != JS_FLOW_DETACHED, "a step differential fixture's flow detached — it holds no top-level await");
        if (r == 0)
            break;
        susp++;
        if (JS_StepMachineCount(rt) > mach) mach = JS_StepMachineCount(rt);
    }
    CHECK(!JS_IsException(res), "a step differential fixture's program threw");
    JS_ToInt32(ctx, &got, res);
    JS_FreeValue(ctx, res);
    JS_FlowFree(ctx, flow);
    *psusp = susp; *pmach = mach;
    return got;
}

static void sort_merge_selftest(JSContext *ctx)
{
    int susp_off = 0, mach_off = 0, susp_on = 0, mach_on = 0;
    int32_t h_off, h_on;

    h_off = tf_step_diff_run(ctx, TF_SORT_SRC, "sort-merge", &TF_DIFF_DECLINE, &susp_off, &mach_off);
    CHECK(h_off >= 0,
          "23.1.3.30 with no comparator sorted 300 distinct values into an order that is not strictly "
          "ascending by 23.1.3.30.2 steps 5-10, or lost one — the merge is wrong before any schedule is "
          "involved, so nothing below this line would mean anything");
    CHECK(susp_off == 0,
          "the flow parked with the policy DECLINING every offer — a yield the policy refuses must re-enter "
          "the machine, not park it, or the offer is a bound wearing a question mark");

    h_on = tf_step_diff_run(ctx, TF_SORT_SRC, "sort-merge", &TF_DIFF_FORCE, &susp_on, &mach_on);
    CHECK(h_on == h_off,
          "the SAME sort of the SAME array answered differently under two schedules — a resume dropped, "
          "duplicated or reordered a merge placement, which is the cap §scheduler's razor names: a yield you "
          "cannot prove is lossless is a cap");
    CHECK(mach_on > 0,
          "the flow never suspended INSIDE a step machine, and the only builtin this program runs is the sort "
          "— so 23.1.3.30.1 step 4's whole sequence of comparisons still runs inside one def->step(): a stretch "
          "of algorithm the size of the page's array that RAM pressure cannot page, a higher-value flow cannot "
          "overtake and the cooperative quantum cannot expire inside");
    CHECK(susp_on > susp_off,
          "forcing every yield produced no more suspensions than declining every one");
    JS_SetFlowControlHooks(&TF_DIFF_NONE);
}

/* ===== A KEYED WALK THE SCHEDULER CAN BE ASKED INSIDE =====
 *
 * The sort above is one machine that declared its own rest points. THIS is the shape that declared none and
 * needed none declared: ECMAScript §23.1.3.2 Array.prototype.concat ( ...items ) walks its source with KEYED
 * REQUESTS — step 5.2.4.2's `exists is ? HasProperty(item, propertyKey)`, step 5.2.4.3.1's `subElement is ?
 * Get(item, propertyKey)`, step 5.2.4.3.2's `Perform ? CreateDataPropertyOrThrow(array, ! ToString(𝔽(nextIndex)),
 * subElement)` — and every one of those returns a request to the driver rather than resolving in place. On a
 * plain array the driver ANSWERS each one itself: no accessor, no trap, nothing user-written runs. So the
 * machine round-tripped the driver three times per element and the scheduler was never once asked, which is a
 * span the length of the receiver that RAM pressure cannot page and a higher-value flow cannot overtake.
 *
 * THE MACHINE IS NOT TOUCHED BY THE FIX AND THAT IS THE POINT — the offer is made where every re-entry
 * converges (do_step_step), so a walk built out of requests becomes parkable without declaring anything. This
 * fixture is what says so: it is written against a machine that returns JS_STEP_YIELD nowhere, and before the
 * offer moved there it could not park inside one however hard the policy pushed.
 *
 * The program checks its own answer for the reason the sort's does: concat's result is a KNOWN permutation
 * (0..2n-1 in order), so a resume that dropped, duplicated or reordered one element fails the positional test,
 * and the checksum pins which. -1 is "concat was wrong", a different failure from "the two schedules disagree".
 * `concat` is the program's ONLY builtin call, which is what makes a live step machine at a suspension
 * attributable to it — the loops and the comparison are operators the interpreter runs itself. */
static const char *TF_CONCAT_SRC =
    "(function(){ var n = 200, x = [], y = [], i, h = 0, ok = 1, a;"
    " for (i = 0; i < n; i++) x[i] = i;"
    " for (i = 0; i < n; i++) y[i] = n + i;"
    " a = x.concat(y);"
    " if (a.length !== 2 * n) ok = 0;"
    " for (i = 0; i < 2 * n; i++) {"
    "   if (a[i] !== i) ok = 0;"
    "   h = (h * 31 + a[i]) % 1000003; }"
    " return ok ? h : -1; })()";

static void concat_keyed_selftest(JSContext *ctx)
{
    int susp_off = 0, mach_off = 0, susp_on = 0, mach_on = 0;
    int32_t h_off, h_on;

    h_off = tf_step_diff_run(ctx, TF_CONCAT_SRC, "concat-keyed", &TF_DIFF_DECLINE, &susp_off, &mach_off);
    CHECK(h_off >= 0,
          "§23.1.3.2 Array.prototype.concat of two dense 200-element arrays did not produce 0..399 in order — "
          "concat is wrong before any schedule is involved, so nothing below this line would mean anything");
    CHECK(susp_off == 0,
          "the flow parked with the policy DECLINING every offer — an offer the policy refuses must re-enter "
          "the machine, not park it, or the offer is a bound wearing a question mark");

    h_on = tf_step_diff_run(ctx, TF_CONCAT_SRC, "concat-keyed", &TF_DIFF_FORCE, &susp_on, &mach_on);
    CHECK(h_on == h_off,
          "the SAME concat of the SAME arrays answered differently under two schedules — a resume dropped, "
          "duplicated or reordered an element, or delivered a keyed request's answer to the wrong call site, "
          "which is the cap §scheduler's razor names: a yield you cannot prove is lossless is a cap");
    CHECK(mach_on > 0,
          "the flow never suspended INSIDE a step machine, and §23.1.3.2's walk is the only builtin this "
          "program runs — so its per-element HasProperty/Get/CreateDataPropertyOrThrow still round-trips the "
          "driver with the scheduler never asked. That is un-parkable for the length of the receiver, and the "
          "machine declares no yield of its own, so nothing but the driver's own convergence point can offer "
          "the rest point");
    CHECK(susp_on > susp_off,
          "forcing every offer produced no more suspensions than declining every one");
    JS_SetFlowControlHooks(&TF_DIFF_NONE);
}

static void flow_job_selftest(JSContext *ctx)
{
    Flow a, b;
    JSValue e, r;
    int dropped;

    /* THE QUEUE'S OWN CONTEXT, which flow_add names for every real flow (solver/flow.c) and which this
       fixture must name for itself because it builds no flow — the readers hold a Flow and nothing else. */
    pending_set_ctx(ctx);
    memset(&a, 0, sizeof a); a.jobs = JS_UNDEFINED;
    memset(&b, 0, sizeof b); b.jobs = JS_UNDEFINED;
    CHECK(flow_job_pending(&a) == 0 && !flow_job_microtask(&a) && flow_job_external(&a) == 0,
          "an untouched flow's queue must answer empty from its JS_UNDEFINED without ever minting an Array");

    /* ARRIVAL ORDER WITHIN A QUEUE, CHECKPOINT ORDER ACROSS THEM. Queued task 1, microtask 2, task 3,
       microtask 4; §8.1.7.3 "Processing model" performs a microtask checkpoint at the end of each task, so
       both microtasks run before either task and each pair keeps the order it arrived in. */
    tf_job_push_int(ctx, &a, 1, 1);
    tf_job_push_int(ctx, &a, 2, 0);
    tf_job_push_int(ctx, &a, 3, 1);
    tf_job_push_int(ctx, &a, 4, 0);
    CHECK(flow_job_pending(&a) == 4, "four pushes must be four records");
    CHECK(flow_job_microtask(&a), "a queue holding a microtask must say so — the checkpoint is over exactly "
                                  "when it does not");

    /* THE FORK, TAKEN WITH THE PARENT'S QUEUE FULL. Both arms must see their own copy of all four. */
    b.jobs = flow_job_fork(ctx, &a);
    CHECK(flow_job_pending(&b) == 4, "an arm forked over a full queue must inherit every job — dropping one is "
                                     "the work item the WFQ may never drop, and it surfaces only as a reaction "
                                     "that never ran");

    /* MUTATING ONE ARM MUST NOT TOUCH THE OTHER — the whole reason the ARRAY is per-flow while the RECORDS are
       shared. The arm takes two jobs and appends a fifth; the parent must still hold exactly its four. */
    e = flow_job_take(ctx, &b); JS_FreeValue(ctx, e);
    e = flow_job_take(ctx, &b); JS_FreeValue(ctx, e);
    tf_job_push_int(ctx, &b, 9, 1);
    CHECK(flow_job_pending(&b) == 3, "an arm's take and push must land on its own Array");
    CHECK(flow_job_pending(&a) == 4,
          "a sibling arm's consumption changed the parent's queue — the two arms are sharing one Array, so one "
          "timeline's reactions are being run out of another's");

    /* AND THE ORDER, RUN. */
    g_tf_job_n = 0;
    tf_job_run_all(ctx, &a);
    CHECK(g_tf_job_n == 4 && g_tf_job_seen[0] == 2 && g_tf_job_seen[1] == 4 &&
          g_tf_job_seen[2] == 1 && g_tf_job_seen[3] == 3,
          "a task began while the flow still held a microtask — a plain FIFO runs `setTimeout(f, 0)` in the "
          "middle of a promise chain, which is the one ordering §8.1.7's checkpoint exists to forbid");
    CHECK(flow_job_pending(&a) == 0, "a drained queue must be empty");

    /* THE ARM'S RECORDS SURVIVED THE PARENT DRAINING ITS OWN — the shared entries are alive because the arm's
       Array still names them, which is the refcount the fork bought instead of a deep copy. The arm's two
       takes above consumed ITS microtasks (2 then 4, by the same checkpoint rule), so what it holds is the two
       tasks it inherited and the one it appended, in arrival order: 1, 3, 9. That the parent then ran 2 and 4
       out of the SAME records is the sharing — a deep copy would have given each arm its own pair. */
    g_tf_job_n = 0;
    tf_job_run_all(ctx, &b);
    CHECK(g_tf_job_n == 3 && g_tf_job_seen[0] == 1 && g_tf_job_seen[1] == 3 && g_tf_job_seen[2] == 9,
          "an arm ran the wrong records after its parent drained the queue they were forked from");

    /* A SECOND CALLEE, AND A RECORD WITH NO ARGUMENTS AT ALL. The callee is named by an ordinal into a table
       this session mints on first sight, so two distinct callees must not collapse onto one name. */
    flow_job_push(ctx, &a, tf_job_count, 0, NULL, 0, tf_handle_new());
    e = flow_job_take(ctx, &a);
    r = flow_job_run(ctx, e);
    {
        int32_t got = -1;
        JS_ToInt32(ctx, &got, r);
        CHECK(got == 1000, "a job record named the wrong callee, or carried arguments it was never given");
    }
    JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, e);

    /* HTML §7.5.10 "Destroying documents", destroy a document STEP 7 — every task of the destroyed document
       removed WITHOUT running.
       The key is the enqueuing realm, and there is exactly one realm in this fixture, so the whole queue goes
       and the count is the answer document_lifecycle.c asserts on. */
    tf_job_push_int(ctx, &a, 5, 1);
    tf_job_push_int(ctx, &a, 6, 0);
    dropped = flow_job_drop_realm(ctx, &a, ctx);
    CHECK(dropped == 2 && flow_job_pending(&a) == 0,
          "destroy a document step 7 left a destroyed document's tasks queued — each of them runs page code "
          "in a Document whose browsing context is null");
    CHECK(flow_job_drop_realm(ctx, &a, ctx) == 0,
          "a second drop of the same realm found more — the walk is not seeing all of the queue");

    /* REMOVAL BY NAME, AND THE FORK'S TWO COPIES OF ONE HANDLE. HTML §4.11.4 "The dialog element", step 1 of
       queue a dialog toggle event task — "Remove element's dialog toggle task tracker's task from its task
       queue" — is why a record carries the name the enqueue issued. The property exercised here is the one
       that makes the hook ask the RUNNING flow alone: an arm names the SAME records, and each arm's tracker
       names its own copy, so a removal made in one timeline must leave the other's task queued. */
    {
        Flow c;
        JSTaskHandle h_keep = tf_handle_new(), h_go = tf_handle_new();
        JSValue v20 = JS_NewInt32(ctx, 20), v21 = JS_NewInt32(ctx, 21);

        memset(&c, 0, sizeof c); c.jobs = JS_UNDEFINED;
        flow_job_push(ctx, &a, tf_job_note, 1, (JSValueConst *)&v20, 1, h_keep);
        flow_job_push(ctx, &a, tf_job_note, 1, (JSValueConst *)&v21, 1, h_go);
        JS_FreeValue(ctx, v20);
        JS_FreeValue(ctx, v21);
        c.jobs = flow_job_fork(ctx, &a);
        CHECK(flow_job_remove(&a, h_go) == 1 && flow_job_pending(&a) == 1,
              "a queued task named by its handle was not taken off the queue — a toggle task tracker then "
              "cannot coalesce two transitions into one event, and the second `toggle` fires carrying a state "
              "the element has already left");
        CHECK(flow_job_remove(&a, h_go) == 0,
              "removing an already-removed task answered anything but 0 — a handle outlives what it names, so "
              "finding nothing is the ordinary answer and is what a monotone id nobody re-issues is for");
        CHECK(flow_job_pending(&c) == 2,
              "a removal in one timeline took the ARM's copy of the task as well — the two copies are two "
              "timelines' jobs wearing one name, and a hook that swept every flow would delete a sibling's");
        CHECK(flow_job_remove(&c, h_go) == 1 && flow_job_remove(&c, h_keep) == 1 && flow_job_pending(&c) == 0,
              "the arm could not remove the copies it holds under the same two names");
        g_tf_job_n = 0;
        tf_job_run_all(ctx, &a);
        CHECK(g_tf_job_n == 1 && g_tf_job_seen[0] == 20,
              "the removal took the wrong record off the queue — the handle names one job and only one");
        JS_FreeValue(ctx, c.jobs); c.jobs = JS_UNDEFINED;
    }

    /* AN ARGUMENT LIST LONGER THAN THE VECTOR flow_job_run KEEPS ON THE STACK — `setTimeout(f, a, b, …)`
       queues the callee plus every extra argument, so this is a shape the page reaches and the only branch in
       the run path the cases above do not. The record carries them all and the callee must see exactly them:
       an argument vector is where the step machines' two ownership bugs both lived. */
    {
        JSValue many[12];
        int i;

        for (i = 0; i < 12; i++) many[i] = JS_NewInt32(ctx, i);
        flow_job_push(ctx, &a, tf_job_count, 12, (JSValueConst *)many, 0, tf_handle_new());
        for (i = 0; i < 12; i++) JS_FreeValue(ctx, many[i]);
        e = flow_job_take(ctx, &a);
        r = flow_job_run(ctx, e);
        {
            int32_t got = -1;
            JS_ToInt32(ctx, &got, r);
            CHECK(got == 1012, "a job whose arguments did not fit the run path's stack vector reached its "
                               "callee with the wrong count");
        }
        JS_FreeValue(ctx, r);
        JS_FreeValue(ctx, e);
    }

    /* THE PROVENANCE BRACKET — what the park reads. Work the replayed program causes is regenerated by the
       replay; work handed in from outside it is not, and only the arrival knows which. */
    tf_job_push_int(ctx, &a, 7, 1);
    CHECK(flow_job_external(&a) == 0, "a job the flow's own code queued must not be marked external");
    flow_job_external_begin();
    tf_job_push_int(ctx, &a, 8, 1);
    flow_job_external_end();
    CHECK(flow_job_external(&a) == 1,
          "a job queued while a routed record was being turned into local work was not marked — the park would "
          "then write a recipe that resumes this document one message short with nothing to say so");

    tf_job_run_all(ctx, &a);
    JS_FreeValue(ctx, a.jobs); a.jobs = JS_UNDEFINED;
    JS_FreeValue(ctx, b.jobs); b.jobs = JS_UNDEFINED;
}


/* ===== A FLOW THAT BLOCKS ON THE HOST, which is the shape a cross-document read has =====
 *
 * `iframe.contentWindow.document.body` must answer at its own call site, and one instance is one document, so
 * the answer is in another instance and not available in this turn. The flow SUSPENDS there — the same
 * snapshot path as an await or a loop back-edge — siblings run, and it resumes with the value.
 *
 * The two things that make that work and are what this exercises: the preempt hook YIELDS a blocked flow
 * regardless of how it ranks (the answer cannot arrive while it holds the thread), and a mid-frame yield while
 * blocked reports the flow HOST-OWED rather than runnable (otherwise the scheduler hands it the thread again
 * immediately and it spins). Without either, this member never returns.
 *
 * It is a STEP MACHINE because that is the only C shape in this engine that can suspend and be re-entered at
 * the same call site — a plain C body would have to answer or throw, and answering is what it cannot do. */
typedef struct { uint32_t req; } HostReqState;

static void hostreq_visit(JSContext *ctx, void *st, JSStepVisit *v) { (void)ctx; (void)st; (void)v; }

static int hostreq_step(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                        JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    HostReqState *s = st;
    JSValueConst answer;

    (void)hdr; (void)out_cb; (void)out_argc;
    JS_FreeValue(ctx, cb_result);
    if (s->req == 0) {
        const char *op = argc > 0 ? JS_ToCString(ctx, argv[0]) : NULL;
        if (!op) return JS_STEP_ABRUPT;
        s->req = engine_host_request(ctx, op);
        JS_FreeCString(ctx, op);
        return JS_STEP_YIELD;   /* park; the blocked yield deschedules the flow until the host answers */
    }
    /* RE-ENTERED. Until the answer lands the machine yields again — and because the flow is blocked, that
       yield is a suspension rather than a spin. */
    if (!engine_host_answered(s->req, &answer)) return JS_STEP_YIELD;
    /* THE ANSWER IS A COMPLETION: a host that answers by relaying a peer's program may answer with a THROW,
       and it is raised here, at the read that parked, exactly as the cross-agent machines raise theirs. */
    {
        int r = engine_host_take_completion(ctx, s->req, presult);
        s->req = 0;
        return r;
    }
}

/* WHERE THIS MACHINE RESTS. It has one stage and rests at it repeatedly: a blocked read yields until the host
   answers, and every one of those yields is a suspension of the flow. The label is the RULE it models rather
   than a clause of ECMAScript — there is no spec algorithm here to cite, and inventing one would be a claim
   about a standard rather than a reference to it (see quickjs.c's B64OP_STAGES). */
#define HOSTREQ_STAGES(X) \
    X(HOSTREQ_BLOCKED, "SECURITY.md's synchronous cross-instance read: the request is placed with the host and " \
                       "the flow is parked at the call site that made it until the answer arrives")
enum { IDL_STEP_STAGE_BASE(HOSTREQ_STAGES) HOSTREQ_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const HOSTREQ_STEPS[] = { HOSTREQ_STAGES(JS_STEP_STAGE_LABEL) NULL };

static const IdlStepDecl HOSTREQ_DECL = { hostreq_step, sizeof(HostReqState), hostreq_visit, NULL,
                                          "a cross-instance read that blocks on the host",
                                          HOSTREQ_STEPS };

/* THE HOST'S SIDE, driven from the fixture's step loop: answer everything outstanding. A real host routes each
   record to the instance holding that document; here the answer is the request text turned back, which is
   enough to prove the value reaches the call site that asked. */
/* Child document ids this fixture has handed out. Starts above the fixture's own document (1). */
static int hostreq_answer_all(JSContext *ctx)
{
    const char *reqs = engine_host_requests();
    const char *p = reqs;
    int n = 0;

    /* NOTICES ARE ONE-WAY. `navigable.create` announces a document the engine named itself, so there is nothing
       to answer; draining it is the whole of this fixture's obligation, and a read that needs the child's
       ACTIVE DOCUMENT still comes through as a request above. */
    (void)engine_host_notices();

    while (*p) {
        const char *tab = strchr(p, '\t');
        const char *end = strchr(p, '\n');
        uint32_t id;
        /* A RECORD THIS LOOP CANNOT PARSE IS NOT A REASON TO STOP ANSWERING THE REST OF THEM. This was a bare
           `break`, and a `break` here abandons every record AFTER the malformed one — silently, and with
           exactly the symptom that has no symptom: those flows stay blocked at the call site that asked, the
           census reports them under `blocked` like any flow that is merely waiting, and nothing anywhere
           distinguishes "the host has not answered yet" from "the host walked past this one and never will".
           engine_host_requests writes `id<TAB>op<NL>` and nothing else, so a record without both is a join
           this host cannot read — an op carrying a newline is how that happens — and it is a defect in the
           record, not a condition to tolerate. */
        DCHECK(tab != NULL && end != NULL,
               "an outstanding host request did not arrive as `id<TAB>op<NL>` — every record after it in this "
               "join goes unanswered, and each of those is a flow suspended mid-frame at the read that asked, "
               "with nothing but a `blocked` count to say it is never coming back");
        if (!tab || !end) break;
        id = (uint32_t)strtoul(p, NULL, 10);
        {
            JSValue v;
            if (!strncmp(tab + 1, "windowproxy.get\t", 16)) {
                /* THE PEER'S HALF of §7.2.5.1's cross-instance [[Get]]. A real host routes this to the
                   instance holding that document and answers under the named world; this fixture stands in for
                   that peer exactly as it stands in for the network, so the SUSPEND/RESUME path is exercised
                   end to end without a second instance. The member is the last tab-separated field. */
                /* BOUNDED TO THIS RECORD. The joined buffer is `id<TAB>op<NL>` repeated, so it is NOT
                   NUL-terminated at the newline — an strrchr from the op start finds the last tab in every
                   record that follows, and the member read was a field of some later request. Scan back from
                   this record's end instead. */
                const char *last = end;
                size_t mlen;
                while (last > tab && last[-1] != '\t') last--;
                mlen = (size_t)(end - last);
                v = (mlen == 6 && !memcmp(last, "closed", 6)) ? JS_FALSE
                  : (mlen == 6 && !memcmp(last, "length", 6)) ? JS_NewInt32(ctx, 0)
                                                              : JS_NULL;
            } else if (!strncmp(tab + 1, "document.fetch\t", 15)) {
                /* §7.4 STEP 14'S NETWORK HALF — this fixture standing in for it exactly as it stands in for
                   the peer above, so the load's suspend/resume runs end to end. `{body, headers}` is ONE
                   answer because everything §7.5.1 creates a Document from is a property of THE RESPONSE.
                   The body is a DOCUMENT WITH A SCRIPT,
                   because the only thing that proves a navigation happened is the loaded document RUNNING. */
                static const char DOC[] =
                    "<!doctype html><html><head></head><body>"
                    "<script>fetch('/api/iframesrc?v=loaded');</script></body></html>";
                v = JS_NewObject(ctx);
                /* AS BYTES: §2.2.5's body is a byte sequence, and a Document is parsed from one.
                   AND AS OWN DEFINES, for the reason core/fetch/fetch.c's fetch_reply_new states in full at
                   its own writes: this record is built by the zone standing in for the trusted host, IN THE
                   HOST'S OWN TIME between two scheduler slices, on an object whose prototype is the PAGE'S
                   `Object.prototype`. `JS_SetPropertyStr` is [[Set]] — ECMA-262 §10.1.9 "[[Set]] ( propertyKey,
                   value, receiver )", which delegates to §10.1.9.2 "OrdinarySetWithOwnDescriptor ( obj,
                   propertyKey, value, receiver, ownDesc )", whose first step is "If ownDesc is undefined, then
                   Let parent be ? obj.[[GetPrototypeOf]]()" — so an accessor the page put on that chain would
                   be CALLED here, where no flow is running and a park may be outstanding, and a frozen
                   prototype would make the write refuse by throwing into a region that has no owner for it. */
                JS_DefinePropertyValueStr(ctx, v, "body",
                                          JS_NewArrayBufferCopy(ctx, (const uint8_t *)DOC, sizeof DOC - 1),
                                          JS_PROP_C_W_E);
                /* THE RESPONSE'S HEADER LIST, as the field lines §7.4 step 14's answer carries — EMPTY here,
                   which is a response that carried no headers and is the statement this fixture can honestly
                   make: it serves these bytes out of a string literal. It was `csp: null`, one narrowed
                   field, which is the shape that kept §7.1.3's opener policy out of a navigated Document. */
                JS_DefinePropertyValueStr(ctx, v, "headers", JS_NewString(ctx, ""), JS_PROP_C_W_E);
            } else {
                v = JS_NewStringLen(ctx, tab + 1, (size_t)(end - tab - 1));
            }
            /* ONE ANSWER PER REQUEST. Answering inside a branch AND here answered twice, which the engine's own
               assert named at the site — a second answer would overwrite a value the asking machine may
               already have read. */
            /* This fixture stands in for the trusted zone and answers out of its own tables, so every answer
               it gives is a NORMAL completion — there is no peer program here to have thrown in one. */
            n += engine_host_answer(ctx, id, NULL, v, ENGINE_COMPLETION_NORMAL, ENGINE_ANSWER_HOST);
            JS_FreeValue(ctx, v);
        }
        p = end + 1;
    }
    return n;
}

/* THE AGENT AND THE DOCUMENT, SPLIT — the same split wpt_runner.c and main.c carry, for the same reason: a
   SAME-ORIGIN CHILD NAVIGABLE IS A SECOND DOCUMENT IN THIS AGENT (HTML's similar-origin window agent is one
   heap), so what a document of this build IS has to be one description that runs twice. This fixture's probes
   read `_cw.parent === window` through exactly that child.

   AND WHAT EACH HALF *IS* IS core/platform.h's ONE LIST, not a copy typed out here. The copy that stood in
   this file claimed, in its own comment, to install "the components the ABI entry installs, so this fixture
   runs the engine that ships" — and was missing twenty-one of them: url, url_search_params, blob, all four
   stream standards, encoding and text_stream, message_port, xml_http_request, broadcast_channel,
   window_message, structured_clone, message_event, error_event, the three File System Access components,
   storage_manager and module_loader. A fixture that runs a smaller browser than the one that ships is
   measuring a different browser, and every @H example and @S PoC it verifies is a claim about that other
   browser. What stays below is what is genuinely this fixture's: its host EDGES and its sinks. */
static int g_id_host_read, g_id_append_child;   /* declared once per agent — a member has one pool entry */

static void tf_agent_init(JSContext *ctx, const char *origin, const char *top_level_url)
{
    static const IdlArgType HR_ARGS[1] = { IDL_DOMSTRING };
    static const IdlArgType ONE_STR[1] = { IDL_DOMSTRING };
    PlatformAgent agent;

    /* THE SYNCHRONOUS HOST READ. A DECLARED step member, because suspending and answering at the same call
       site is the only thing a plain C body cannot do. */
    g_id_host_read = idl_method_id_step(ctx, HR_ARGS, 1, NULL, 0, &HOSTREQ_DECL, 0);
    /* A DECLARED member, like every DOM member — this host-edge mutates the tree, and §4.2.3's insertion steps
       are drained by the machine every declared member converges on. */
    g_id_append_child = idl_method_id(ctx, ONE_STR, 1, js_append_child, 0);
    /* THIS FIXTURE'S EDGES — WHO answers, which is the half that is legitimately per-host. */
    { static const FetchProvider P = { engine_pending_fetch_url }; fetch_set_provider(&P); }
    timer_set_script_sink(engine_queue_script);   /* §8.6: a STRING handler is evaluated, as a flow */

    agent.origin = origin;
    agent.top_level_url = top_level_url;
    /* §7.5.1's requestsOAC. This fixture's document comes from no response at all — it is a string of HTML
       handed to the probe — so there is no `Origin-Agent-Cluster` header to have sent, and false is what
       §8.1.2.2 allocates the cluster with. Stated rather than left to the struct's padding, because the field
       decides an observable (`window.originAgentCluster`) and a fixture that read uninitialized memory for it
       would be flaky in the one direction nobody looks at. */
    agent.requests_oac = false;
    /* §7.1.3's OPENER POLICY, and the same sentence: this fixture's document comes from no response, so there
       is no `Cross-Origin-Opener-Policy` header to have sent and §7.1.3's INITIAL value is what §7.3.2.3
       creates the browsing context group with — leaving its cross-origin isolation mode `none`. Stated rather
       than left to the struct's padding, because it decides `window.crossOriginIsolated` and HR-TIME §4's
       clock resolution, and a fixture reading uninitialized memory for it would be flaky in the one direction
       nobody looks at. */
    agent.opener_policy = OPENER_POLICY_UNSAFE_NONE;
    platform_agent_init(ctx, &agent);
}

/* ONE DOCUMENT — run once per document including the first. THE PLATFORM FIRST, then this fixture's own
   globals: a host ADDS to the one list and cannot subtract from it, and adding afterwards is also what lets
   the `eval` sink below stand where the language's own `eval` would. */
static void tf_realm_install(JSContext *ctx, lxb_html_document_t *dom, const char *url, const char *origin,
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

    /* THE FIXTURE'S OWN SURFACE — the @S sinks and the host-edge stand-ins the probes drive. Every one of
       these is this fixture's, which is why it is here and not in the list. */
    JS_SetPropertyStr(ctx, g, "loadScript", JS_NewCFunction(ctx, js_load_script, "loadScript", 1));   /* lazy-chunk load */
    /* `eval` is NOT installed — see the note where its stand-in used to be defined. The intrinsic is the sink. */
    JS_SetPropertyStr(ctx, g, "setInnerHTML", JS_NewCFunction(ctx, js_html_sink, "setInnerHTML", 1));   /* the innerHTML sink */
    JS_SetPropertyStr(ctx, g, "setLocation", JS_NewCFunction(ctx, js_url_sink, "setLocation", 1));   /* the location/URL sink */
    /* THE SYNCHRONOUS HOST READ. A DECLARED step member, because suspending and answering at the same call
       site is the only thing a plain C body cannot do. */
    idl_install_method(ctx, g, "hostRead", 1, g_id_host_read);
    JS_SetPropertyStr(ctx, g, "setBodyAttr", JS_NewCFunction(ctx, js_set_body_attr, "setBodyAttr", 2));   /* DOM attr write (per-flow) */
    JS_SetPropertyStr(ctx, g, "getBodyAttr", JS_NewCFunction(ctx, js_get_body_attr, "getBodyAttr", 1));   /* DOM attr read (per-flow) */
    /* A DECLARED member, like every DOM member — this host-edge mutates the tree, and §4.2.3's insertion
       steps are drained by the machine every declared member converges on. As a raw JS_CFUNC_DEF its steps
       never ran at all; nothing showed it, because the <span> it appends is neither a script nor a custom
       element. The engine asserts on exactly this now, which is what caught it. */
    idl_install_method(ctx, g, "appendChild", 1, g_id_append_child);
    JS_SetPropertyStr(ctx, g, "lastChildMark", JS_NewCFunction(ctx, js_last_child_mark, "lastChildMark", 0));   /* DOM node read */
    JS_SetPropertyStr(ctx, g, "state", concolic_new(ctx, "{state}", "{state}", JS_UNDEFINED));   /* injected/unknown app state */
    /* AN UNKNOWN CARRYING A NUMERIC EXAMPLE, which is what §13.15.3 step 1.c's arm turns on. Its display form
       is a HOLE so endpoint.c mints a path parameter for it and aligns the computed address against the shape,
       which is how the example is readable at all: a query value is read off the SHAPE. No source this fixture
       already reaches has both halves — `screen.width` carries a Number and displays unbraced, `{state}`
       displays as a hole and carries no example. */
    JS_SetPropertyStr(ctx, g, "num", concolic_new(ctx, "{num}", "{num}", JS_NewInt32(ctx, 1920)));
    /* INDEXED DATABASE §2.7/§2.8's two operations, until §4.6/§4.9/§5.1 exist to state them as members. */
    JS_FreeValue(ctx, g);
}

/* A SAME-ORIGIN CHILD NAVIGABLE'S REALM — a second JSContext in the SAME JSRuntime. */
static JSContext *tf_child_realm(JSRuntime *rt, lxb_html_document_t *dom, const char *url,
                                 const char *top_level_url, const char *origin, const char *csp,
                                 const char *csp_self_origin, SandboxFlags sandbox_flags, uint32_t doc_id,
                                 JSValueConst nav_proxy)
{
    JSContext *ctx = JS_NewContext(rt);

    CHECK(ctx != NULL, "a same-origin child navigable's realm could not be created");
    /* §3.7: a realm gets its OWN intrinsics — the members on them run in the realm that DEFINED them, so a
       child sharing the agent realm's EventTarget.prototype would resolve every unqualified
       `addEventListener` against the PARENT's window. They come from the ONE list the components declared
       themselves into, so a component added anywhere is installed in every realm with no host to edit.
       §7.4 decided this child's top-level creation URL and handed it over — using `url` would make an
       about:blank iframe of an http page a secure context. */
    realm_install_intrinsics(ctx, top_level_url);
    tf_realm_install(ctx, dom, url, origin, csp, csp_self_origin, sandbox_flags, doc_id, nav_proxy);
    return ctx;
}

/* INDEXED DATABASE §2.1's DATABASE AND §2.2's OBJECT STORE — the state a store IS, before anything schedules
 * an operation over it.
 *
 * §6.1 AND §6.2 ARE NOT HERE ANY MORE, AND THAT IS NOT A GAP. §6.1 became a delegatable algorithm the moment
 * its step 5 was built: that step runs §7.1's extract-a-key once per index which references the store, whose
 * step 3 is §7.4, and §7.4's array arm exists in exactly ONE form — the parkable walk. So §6.1 is driven by a
 * step machine and there is no C entry for it, deliberately: a C entry would exist only because this fixture is
 * in C, which is CLAUDE.md's own test for a FALLBACK rather than routing (delete the thing it selects against
 * and it becomes meaningless), and it would be the second non-suspending driver §C-stack names as the dual
 * system. The round trip therefore runs where a page runs it — `_s.put`/`_s.get`/`_o.add`/`_at.abort()` in the
 * full document's own statement, read back by the `idb-open` and `idb-record` probes — which is a STRONGER
 * assertion than this file could make, because it goes through §5.6's request, §5.9's success event and §5.5's
 * abort rather than calling the operations directly.
 *
 * WHAT STAYS HERE IS WHAT NEEDS NO FLOW: §2.1's set of databases, §2.2's set of object stores, and the three
 * algorithms below whose own C entries crash exactly where a flow would be required (§7.1's extract, §2.5's key
 * path, §5.5 step 2's revert of the metadata changes, §5.8's abort of an upgrade transaction). */
static int32_t idb_selftest_int(JSContext *ctx, JSValueConst v)
{
    int32_t n = -1;

    CHECK(JS_ToInt32(ctx, &n, v) == 0, "a value this fixture stored as a number did not come back as one");
    return n;
}

/* A TRANSACTION IN THE MODE §2.7 REQUIRES FOR THE CHANGE ABOUT TO BE MADE, over the connection §5.1 would have
   opened. §2.7's first sentence is why the fixture needs one at all — "whenever data is read or written to the
   database it is done by using a transaction" — and every algorithm below takes it as an operand because that
   is who §5.5 step 2 reverts the change for. `stores` is the scope: empty for an upgrade transaction, which is
   what §5.7 step 3 gives one for a database that has no object stores yet. OWNED. */
static JSValue idb_selftest_tx(JSContext *ctx, JSValueConst conn, JSValueConst store, int mode)
{
    JSValue scope = JS_NewArray(ctx);

    CHECK(!JS_IsException(scope), "the fixture's transaction scope could not be allocated");
    if (!JS_IsUndefined(store))
        JS_DefinePropertyValueUint32(ctx, scope, 0, JS_DupValue(ctx, store), JS_PROP_C_W_E);
    return idb_transaction_new(ctx, conn, scope, mode, IDB_DUR_DEFAULT);
}

/* §5.4's END OF A TRANSACTION, without the task that fires `complete`. The fixture is at the pre-boot baseline
   and no flow is running, so there is nothing a database task could be queued onto — and what the assertions
   need from the ending is the one thing §2.7 states about it: "a transaction is said to be LIVE from when it is
   created until its state is set to finished", so the next transaction this fixture creates is not blocked by
   §2.7.2 on an overlapping scope. */
static void idb_selftest_finish(JSContext *ctx, JSValue tx)   /* CONSUMES tx */
{
    idb_transaction_set_state(ctx, tx, IDB_TX_FINISHED);
    JS_FreeValue(ctx, tx);
}

/* INDEXED DATABASE §5.5 STEP 2 — "all the changes made to the database by the transaction are reverted. For
 * upgrade transactions this includes changes to the set of object stores and indexes, as well as the change to
 * the version. Any object stores and indexes which were created during the transaction are now considered
 * deleted for the purposes of other algorithms."
 *
 * WHAT IT EXERCISES: the METADATA changes, which are the ones no flow is needed to make — a store CREATED (it
 * leaves the set and is marked deleted, which is what a handle the page still holds must report), a store
 * RENAMED (it is filed under its former name again), a store DESTROYED (it is back in the set and no longer
 * deleted), and the VERSION. The RECORD changes are reverted where they are made: §6.1 is a step machine, so
 * the document's own `_at.abort()` asserts them through §5.5 itself rather than through this call.
 *
 * AND THE ORDER, which is the part a one-change test cannot reach. The changes below are made as version →
 * create → rename → destroy, so the revert has to run them BACKWARDS: the destroy is undone first, putting the
 * store back under the name the rename gave it, which is the only state in which undoing the rename can find
 * it. A revert that replayed forwards would look right for any single change and would leave this store filed
 * under a name it never had.
 *
 * IT REVERTS RATHER THAN ABORTING because §5.5's other six steps are a queued database task and an event
 * dispatch, and this fixture runs at the pre-boot baseline where no flow is running to queue one onto. What it
 * exercises is what it claims: the changes, and the writes that put them back. */
static void idb_revert_selftest(JSContext *ctx, JSValueConst conn, JSValueConst db, JSValueConst store)
{
    JSValue tx, other, found;

    /* THE UPGRADE TRANSACTION'S THREE, made in an order whose revert only composes backwards. */
    tx = idb_selftest_tx(ctx, conn, JS_UNDEFINED, IDB_TX_VERSIONCHANGE);
    idb_database_set_version(ctx, tx, db, 7);
    other = idb_object_store_create(ctx, tx, db, "t", JS_NULL, false);
    idb_object_store_rename(ctx, tx, db, store, "s2");
    idb_object_store_destroy(ctx, tx, db, store);
    CHECK(idb_database_version(ctx, db) == 7, "§5.7 step 8's write did not reach the database");
    found = idb_object_store_find(ctx, db, "t");
    CHECK(JS_VALUE_GET_PTR(found) == JS_VALUE_GET_PTR(other),
          "§4.4's createObjectStore did not file the store in its database's set");
    JS_FreeValue(ctx, found);

    idb_database_revert_transaction(ctx, tx);

    CHECK(idb_database_version(ctx, db) == 0,
          "§5.7 step 8: \"this change is considered part of the transaction, and so if the transaction is "
          "aborted, this change is reverted\" — the version this database was CREATED with is what it goes "
          "back to, which is also the answer §5.8 step 3 needs for a database an open newly created");
    found = idb_object_store_find(ctx, db, "t");
    CHECK(JS_IsNull(found), "§5.5 step 2 left an object store the transaction CREATED in its database's set");
    JS_FreeValue(ctx, found);
    CHECK(idb_object_store_is_deleted(ctx, other),
          "\"any object stores and indexes which were created during the transaction are now considered "
          "DELETED for the purposes of other algorithms\" — §4.5's members answer out of the handle the page "
          "still holds, so the store record itself has to say so");
    found = idb_object_store_find(ctx, db, "s2");
    CHECK(JS_IsNull(found), "§5.5 step 2 left the store filed under the name the transaction RENAMED it to");
    JS_FreeValue(ctx, found);
    found = idb_object_store_find(ctx, db, "s");
    CHECK(JS_VALUE_GET_PTR(found) == JS_VALUE_GET_PTR(store),
          "§5.5 step 2 did not put the store back in its database's set under the name it had — the destroy "
          "and the rename are undone in that order, and only that order can find it");
    JS_FreeValue(ctx, found);
    CHECK(!idb_object_store_is_deleted(ctx, store),
          "§5.5 step 2 left a store the transaction DESTROYED marked as deleted — every member of §4.5 asks "
          "that question first, so the store would answer an InvalidStateError for a deletion that was undone");
    JS_FreeValue(ctx, other);
    idb_selftest_finish(ctx, tx);
}

/* INDEXED DATABASE §2.5's LIST KEY PATH — its VALIDITY, and §4.5's conversion of a store's list back out.
 *
 * The list arm is the one §2.5 bullet whose rule is not the string rule applied N times: "a NON-EMPTY list
 * containing only strings conforming to the above requirements". So `[]` is refused where `['']` is accepted
 * (the empty STRING is a valid key path, naming the value itself), and a nested list arrives already
 * ToString'd by Web IDL §3.2.21's sequence conversion — `['multi_array', ['a','b']]` is `['multi_array', 'a,b']` and is
 * refused for the comma. Every one of these is a WPT case (keypath_invalid, keypath).
 *
 * AND THE CONVERSION IS A COPY, ASSERTED FROM BOTH SIDES: it is not the store's own record (a page handed that
 * could rewrite a live store's key path), and it is not the same Array twice — Web IDL §3.2.21's steps are
 * "let A be a new Array object created as if by the expression []", so the same-instance identity §4.5's note
 * requires belongs to §2.2.1's HANDLE and not to this. It is EXTENSIBLE, which is the executable form of the
 * claim that a sequence is not a FrozenArray. */
static JSValue idb_selftest_list(JSContext *ctx, const char *const *items, uint32_t n)
{
    JSValue a = JS_NewArray(ctx);
    uint32_t i;

    CHECK(!JS_IsException(a), "the fixture's key-path list could not be allocated");
    for (i = 0; i < n; i++)
        JS_DefinePropertyValueUint32(ctx, a, i, JS_NewString(ctx, items[i]), JS_PROP_C_W_E);
    return a;
}

static void idb_selftest_path_valid(JSContext *ctx, JSValue path, bool expect, const char *why)  /* CONSUMES */
{
    CHECK(idb_key_path_value_is_valid(ctx, path) == expect, why);
    JS_FreeValue(ctx, path);
}

static void idb_key_path_selftest(JSContext *ctx, JSValueConst conn, JSValueConst db)
{
    static const char *const AB[] = { "a", "b" };
    static const char *const SPACED[] = { "array with space" };
    static const char *const EMPTY_ENTRY[] = { "" };
    static const char *const STRINGIFIED[] = { "multi_array", "a,b" };
    JSValue tx, store, first, second, held, entry;
    const char *s;

    idb_selftest_path_valid(ctx, JS_NewString(ctx, "a.b"), true,
                            "§2.5: \"a string consisting of two or more identifiers separated by periods\"");
    idb_selftest_path_valid(ctx, JS_NewString(ctx, ".a"), false,
                            "§2.5: a leading period leaves an empty identifier, which is no key path");
    idb_selftest_path_valid(ctx, idb_selftest_list(ctx, AB, 2), true,
                            "§2.5's last bullet: a non-empty list of valid string key paths IS a key path");
    idb_selftest_path_valid(ctx, idb_selftest_list(ctx, AB, 0), false,
                            "§2.5's last bullet is a NON-EMPTY list, and the emptiness is the LIST's own rule "
                            "— a loop over the entries answers true for the empty list, since there is no "
                            "entry to refuse it");
    idb_selftest_path_valid(ctx, idb_selftest_list(ctx, SPACED, 1), false,
                            "§2.5's note — \"spaces are not allowed within a key path\" — applies to every "
                            "entry of a list and not only to a key path spelled as one string");
    idb_selftest_path_valid(ctx, idb_selftest_list(ctx, EMPTY_ENTRY, 1), true,
                            "§2.5's list holds \"strings conforming to the ABOVE requirements\", the first of "
                            "which is the empty string — WPT's keypath.any.js states what it then means: "
                            "\"[''] uses value as [key]\"");
    idb_selftest_path_valid(ctx, idb_selftest_list(ctx, STRINGIFIED, 2), false,
                            "a nested list arrives already ToString'd by §3.2.21's sequence conversion, so "
                            "['multi_array', ['a','b']] is refused for the COMMA in 'a,b' — a validity that "
                            "looked for nesting instead would accept it");

    tx = idb_selftest_tx(ctx, conn, JS_UNDEFINED, IDB_TX_VERSIONCHANGE);
    store = idb_object_store_create(ctx, tx, db, "kp", idb_selftest_list(ctx, AB, 2), false);
    first = idb_object_store_key_path_value(ctx, store);
    second = idb_object_store_key_path_value(ctx, store);
    held = idb_object_store_key_path(ctx, store);
    CHECK(JS_IsArray(first) && JS_IsArray(second),
          "§4.5: a list key path is \"converted as ... a sequence<DOMString> (if a list of strings)\", and Web "
          "IDL §3.2.21 makes that an Array");
    CHECK(JS_VALUE_GET_PTR(first) != JS_VALUE_GET_PTR(held),
          "§4.5's keyPath answered with the object store's OWN record. \"The returned value is not the same "
          "instance that was used when the object store was created\" and \"changing the properties of the "
          "object has no effect on the object store\" — a page holding that record could rewrite the key path "
          "of a live store");
    CHECK(JS_VALUE_GET_PTR(first) != JS_VALUE_GET_PTR(second),
          "§4.5's conversion answered the SAME Array twice. Web IDL §3.2.21's own steps mint one per "
          "conversion, so the identity §4.5's note requires (\"the same object instance every time it is "
          "inspected\") is the handle's cache — a conversion that memoised here would hand every handle of "
          "this store one Array, which is the half of that note WPT's idbobjectstore_keyPath denies");
    CHECK(JS_IsExtensible(ctx, first) == 1,
          "§4.5's keyPath answered a NON-EXTENSIBLE Array. Web IDL §3.2.21 converts a sequence to a plain "
          "Array; `FrozenArray<T>` is a different parameterized type (§3.2.27) that neither this attribute's "
          "declaration (`readonly attribute any keyPath`) nor §4.5's prose names, so freezing it would be a "
          "property of the answer that no sentence of either standard asks for");
    entry = JS_GetPropertyUint32(ctx, first, 1);
    s = JS_ToCString(ctx, entry);
    CHECK(s != NULL && strcmp(s, "b") == 0,
          "§4.5's conversion did not carry the store's key path across in order");
    JS_FreeCString(ctx, s);
    JS_FreeValue(ctx, entry);
    JS_FreeValue(ctx, held);
    JS_FreeValue(ctx, second);
    JS_FreeValue(ctx, first);
    JS_FreeValue(ctx, store);
    idb_selftest_finish(ctx, tx);
}

/* INDEXED DATABASE §7.1 — EXTRACT A KEY FROM A VALUE USING A KEY PATH, which is the whole of what IN-LINE KEYS
 * are: a store created with a key path files each record under a key read OUT of the value, so `put` is called
 * with no key at all. It is asserted here rather than only through §4.5's `put` because its three answers are
 * three different sentences of that member (a key, a "DataError" for `invalid`, and a "DataError" for `failure`
 * ONLY when the store has no key generator), and a fixture that went through `put` could see the first two
 * collapse into one and not notice.
 *
 * WHAT EACH CASE IS FOR. The empty key path is §7.1's own first step and names the VALUE ITSELF — asserted from
 * both sides, since the value is a key when it is a string and `invalid` when it is a plain object. `length` is
 * asserted over a string containing a SUPPLEMENTARY character, because "the number of ELEMENTS in value" counts
 * UTF-16 code units and the engine holds the string as UTF-8: a walk that counted bytes answers 5 where a
 * browser answers 3, and the record goes into the wrong place in §2.2's sorted list. An own property holding
 * `undefined` is `failure` and not a key, which is a different statement from the property being absent and the
 * one §7.1 spells separately. And the LIST arm's assertion is its ABORT: a compound key is the tuple of all its
 * parts, so a value missing one has no key at all rather than a shorter one.
 *
 * THE TAINTED CASES ARE THE HALF A BROWSER'S OWN SUITE CANNOT HAVE. A bundle stores what it received —
 * `store.put(await res.json())` — so the value, or a field of it, is unknown external input. §7.1 asks "does it
 * have this own property" and "what is at it", and answering `failure` for an unknown would FORCE A BRANCH on
 * attacker-controlled input, toward the arm that takes the record out of the store and loses the code path and
 * the taint together. So a field that IS a concolic reaches §7.4 as one, and a VALUE that is a concolic answers
 * through its own exotic [[Get]] under the field-path identity ("{reply}.id") — which is what the pinned case
 * asserts, because a pin is keyed by exactly that composed path. */
static void idb_extract_case(JSContext *ctx, JSValue value, JSValue key_path, IdbKeyPathResult expect,
                             const char *why)   /* CONSUMES value and key_path */
{
    JSValue key = JS_UNDEFINED;

    CHECK(idb_key_path_extract(ctx, value, key_path, &key) == expect, why);
    JS_FreeValue(ctx, key);
    JS_FreeValue(ctx, key_path);
    JS_FreeValue(ctx, value);
}

/* THE KEY §7.1 EXTRACTED, as §7.3 hands it back — which is how a key's own value is inspected anywhere else in
   this engine, rather than by reaching into the key record this fixture does not own. CONSUMES both. */
static JSValue idb_extract_value(JSContext *ctx, JSValue value, JSValue key_path, const char *why)
{
    JSValue key = JS_UNDEFINED, out;

    CHECK(idb_key_path_extract(ctx, value, key_path, &key) == IDB_KEY_PATH_KEY, why);
    out = idb_key_to_value(ctx, key);
    JS_FreeValue(ctx, key);
    JS_FreeValue(ctx, key_path);
    JS_FreeValue(ctx, value);
    return out;
}

static void idb_extract_string(JSContext *ctx, JSValue got, const char *expect, const char *why)  /* CONSUMES */
{
    const char *s = JS_ToCString(ctx, got);

    CHECK(s != NULL && strcmp(s, expect) == 0, why);
    JS_FreeCString(ctx, s);
    JS_FreeValue(ctx, got);
}

static JSValue idb_extract_obj(JSContext *ctx, const char *field, JSValue v)   /* CONSUMES v */
{
    JSValue o = JS_NewObject(ctx);

    CHECK(!JS_IsException(o), "the fixture's value could not be allocated");
    JS_SetPropertyStr(ctx, o, field, v);
    return o;
}

static void idb_extract_selftest(JSContext *ctx)
{
    static const char *const AB[] = { "a", "b" };
    JSValue v, tainted, got;

    /* "Let hop be ! HasOwnProperty(value, identifier) ... let value be ! Get(value, identifier)" — one segment,
       then two. */
    got = idb_extract_value(ctx, idb_extract_obj(ctx, "id", JS_NewInt32(ctx, 7)), JS_NewString(ctx, "id"),
                            "§7.1 could not extract the key at a one-identifier key path");
    CHECK(idb_selftest_int(ctx, got) == 7, "§7.1 extracted the wrong value at `id`");
    JS_FreeValue(ctx, got);
    v = idb_extract_obj(ctx, "b", JS_NewString(ctx, "z"));
    idb_extract_string(ctx, idb_extract_value(ctx, idb_extract_obj(ctx, "a", v), JS_NewString(ctx, "a.b"),
                                              "§7.1 could not walk a two-identifier key path"),
                       "z", "§7.1 walked `a.b` to the wrong value");

    /* "If keyPath is the empty string, return value and skip the remaining steps." The split of "" is one EMPTY
       identifier and not none, so a walk that reached it would look for a property named "". */
    idb_extract_string(ctx, idb_extract_value(ctx, JS_NewString(ctx, "k"), JS_NewString(ctx, ""),
                                              "§7.1's empty key path names the VALUE ITSELF, and a string is a "
                                              "key"),
                       "k", "§7.1's empty key path answered with something other than the value");
    idb_extract_case(ctx, JS_NewObject(ctx), JS_NewString(ctx, ""), IDB_KEY_PATH_INVALID,
                     "§7.1 over the empty key path answers with the value, and a plain object is not a key — "
                     "\"if key is 'invalid value' or 'invalid type', return INVALID\", which is a different "
                     "answer from `failure` and a different sentence of §4.5");

    /* The three ways a walk ends in `failure`, which §4.5 reports as a DataError only for a store with no key
       generator — the absent property, the absent property one level down, and the own property that is there
       and holds `undefined`. */
    idb_extract_case(ctx, idb_extract_obj(ctx, "a", JS_NewInt32(ctx, 1)), JS_NewString(ctx, "b"),
                     IDB_KEY_PATH_FAILURE, "§7.1: \"if hop is false, return failure\"");
    idb_extract_case(ctx, idb_extract_obj(ctx, "a", JS_NewObject(ctx)), JS_NewString(ctx, "a.b"),
                     IDB_KEY_PATH_FAILURE, "§7.1: a key path whose LAST identifier is absent is failure, and "
                                           "the walk reached it through a segment that resolved");
    idb_extract_case(ctx, idb_extract_obj(ctx, "a", JS_UNDEFINED), JS_NewString(ctx, "a"),
                     IDB_KEY_PATH_FAILURE, "§7.1: \"if value is undefined, return failure\" — an own property "
                                           "holding undefined is a DIFFERENT state from an absent one and the "
                                           "standard spells it as its own step");

    /* "If Type(value) is String, and identifier is 'length': let value be a Number equal to the NUMBER OF
       ELEMENTS in value." U+1D11E is one code POINT, two UTF-16 code units and four UTF-8 bytes, so `a𝄞`
       answers 3 — a walk counting the engine's own bytes would answer 5. */
    got = idb_extract_value(ctx, JS_NewString(ctx, "a\xF0\x9D\x84\x9E"), JS_NewString(ctx, "length"),
                            "§7.1's String `length` arm did not answer with a key");
    CHECK(idb_selftest_int(ctx, got) == 3,
          "§7.1's String `length` counted something other than UTF-16 CODE UNITS — the elements of a JS string "
          "are its code units, and a key that counted UTF-8 bytes files the record in the wrong place in "
          "§2.2's sorted list");
    JS_FreeValue(ctx, got);
    got = idb_extract_value(ctx, idb_extract_obj(ctx, "s", JS_NewString(ctx, "xy")),
                            JS_NewString(ctx, "s.length"),
                            "§7.1's String `length` arm is reached MID-WALK too, not only at a bare key path");
    CHECK(idb_selftest_int(ctx, got) == 2, "§7.1's String `length` answered the wrong count mid-walk");
    JS_FreeValue(ctx, got);

    /* "If value is an Array and identifier is 'length': let value be ! ToLength(! Get(value, 'length'))." */
    v = JS_NewArray(ctx);
    CHECK(!JS_IsException(v), "the fixture's array could not be allocated");
    JS_DefinePropertyValueUint32(ctx, v, 0, JS_NewInt32(ctx, 5), JS_PROP_C_W_E);
    JS_DefinePropertyValueUint32(ctx, v, 1, JS_NewInt32(ctx, 6), JS_PROP_C_W_E);
    got = idb_extract_value(ctx, idb_extract_obj(ctx, "a", v), JS_NewString(ctx, "a.length"),
                            "§7.1's Array `length` arm did not answer with a key");
    CHECK(idb_selftest_int(ctx, got) == 2, "§7.1's Array `length` answered the wrong length");
    JS_FreeValue(ctx, got);

    /* §7.1's LIST ARM, at its abort: "if key is failure, ABORT THE OVERALL ALGORITHM and return failure". The
       value has `a` and not `b`, so the compound key does not exist — it is not the one-element key `[1]`.
       The SUCCEEDING list arm assembles an Array and hands it to §7.4's array arm, which is not built and
       crashes by name; that conversion is the next subproblem and is why only the abort is asserted here. */
    idb_extract_case(ctx, idb_extract_obj(ctx, "a", JS_NewInt32(ctx, 1)), idb_selftest_list(ctx, AB, 2),
                     IDB_KEY_PATH_FAILURE,
                     "§7.1's list arm must abort the OVERALL algorithm when one of its items evaluates to "
                     "failure — a compound key is the tuple of all its parts, and a value missing one has no "
                     "key rather than a shorter one");

    /* A TAINTED FIELD: the ordinary shape of a bundle storing what a reply gave it. §7.4's concolic arm takes
       the key's TYPE from the example and carries the concolic itself as the value, so §7.3 hands back the same
       symbol every constraint the flow narrowed it with names. */
    tainted = concolic_new(ctx, "{reply}.id", "{reply}.id", JS_NewString(ctx, "u-42"));
    got = idb_extract_value(ctx, idb_extract_obj(ctx, "id", JS_DupValue(ctx, tainted)), JS_NewString(ctx, "id"),
                            "§7.1 refused a value whose key-path field is unknown external input — a store "
                            "that did that would take the whole tainted-key surface out of reach");
    CHECK(JS_VALUE_GET_PTR(got) == JS_VALUE_GET_PTR(tainted),
          "§7.1 extracted a tainted field as a LAUNDERED copy — the key a record is filed under has to carry "
          "the fact an attacker chose it, so every later comparison on it forks");
    JS_FreeValue(ctx, got);
    JS_FreeValue(ctx, tainted);

    /* A TAINTED VALUE — `store.put(await res.json())` where the reply itself is unknown. The walk reaches the
       concolic's own exotic [[Get]], which mints the DERIVED source "{reply}.id"; a flow whose gate pinned that
       source reads the pinned string, and asserting the pin is what proves the field path was composed rather
       than the read having gone somewhere else. The pin map is the running flow's constraint and this fixture
       runs at the pre-boot baseline where no flow has narrowed anything, so it is cleared after. */
    /* THE ROOT IS THE SECOND ARGUMENT AND IT IS `{reply}`, NOT the field path: a derivation mints a new
       injection identity and never a new delivery root, so the bytes this pin is about entered through the
       reply that carried the object. */
    concolic_pin("{reply}.id", "{reply}", "u-7");
    idb_extract_string(ctx, idb_extract_value(ctx, concolic_new(ctx, "{reply}", "{reply}", JS_UNDEFINED),
                                              JS_NewString(ctx, "id"),
                                              "§7.1 answered `failure` for an unknown VALUE — that forces a "
                                              "branch on attacker-controlled input, toward the arm that takes "
                                              "the record out of the store entirely"),
                       "u-7", "§7.1 read a concolic value's field under the wrong source identity — the "
                              "derived source is the composed field path, which is what an @S candidate "
                              "delivery and a gate's pin are both keyed by");
    concolic_clear_pins();
}

/* WHAT A HANDLE ANSWERS AFTER THE REVERT — the name of a handle is checked with, and against, the name of the
   store it is a handle for. */
static void idb_selftest_handle_named(JSContext *ctx, JSValueConst handle, const char *expect, const char *why)
{
    JSValue name = idb_object_store_handle_name(ctx, handle);
    const char *got = JS_ToCString(ctx, name);

    CHECK(got != NULL, "an object store handle could not report its own name");
    CHECK(strcmp(got, expect) == 0, why);
    JS_FreeCString(ctx, got);
    JS_FreeValue(ctx, name);
}

/* INDEXED DATABASE §5.8 — ABORT AN UPGRADE TRANSACTION, the other half of §5.5 step 2's sentence: step 2 puts
 * the DATABASE back and this puts back what the page can still SEE of it.
 *
 * WHAT IT EXERCISES, and each of the three is a different arm. (1) §5.8 step 3, the connection's version:
 * §5.1 step 9 set it to the version being opened, §5.7 step 8 raised the database's, and after the revert the
 * connection has to be back at the database's — which for a database an open newly created is the 0 §2.1
 * creates one with, so both of the step's arms are the one read. (2) §5.8 step 5.1 for a store that OUTLIVED
 * the transaction: its handle takes the store's name back, and the store's name is the one step 2 restored.
 * (3) THE GUARD on that step, for a store the transaction CREATED: its handle keeps the name it has, because
 * there is no earlier name to go back to — the assertion is that the two stores answer DIFFERENTLY, which a
 * fixture with only one of them cannot see.
 *
 * HOW A HANDLE'S NAME COMES TO DIFFER FROM ITS STORE'S. A page reaches that state through §4.5's `name` setter,
 * whose last two steps write both ("set store's name to name. Set this's name to name"), after which §5.5 step
 * 2 puts back only the store's. This fixture reaches it through §2.2.1's other sentence — "a name, which is
 * initialized to the name of the associated object store WHEN THE OBJECT STORE HANDLE IS CREATED" — by
 * renaming the store first and minting the handle afterwards. The state under test is identical and it is the
 * state step 5.1 exists for; what differs is only which of the two writes put the handle there.
 *
 * IT REVERTS AND THEN RUNS §5.8, in §5.5's own order, rather than calling abort a transaction: §5.5's steps 6
 * and 7 are a queued database task and an event dispatch, and this fixture runs at the pre-boot baseline where
 * there is no flow to queue one onto — the same reason idb_revert_selftest above stops where it does. */
static void idb_upgrade_abort_selftest(JSContext *ctx, JSValueConst conn, JSValueConst db, JSValueConst store)
{
    JSValue tx, made, kept_handle, made_handle, found;

    /* §5.1 step 9 ("set connection's version to version") and §5.7 step 8 ("set db's version to version"),
       which is the state every upgrade is in by the time its transaction can abort. */
    idb_connection_set_version(ctx, conn, 3);
    tx = idb_selftest_tx(ctx, conn, JS_UNDEFINED, IDB_TX_VERSIONCHANGE);
    idb_database_set_version(ctx, tx, db, 3);

    /* The store that EXISTED before this transaction, renamed inside it — §5.8 step 5.1's own case. */
    idb_object_store_rename(ctx, tx, db, store, "renamed");
    kept_handle = idb_object_store_handle(ctx, store, tx);
    idb_transaction_handle_add(ctx, tx, kept_handle);
    idb_selftest_handle_named(ctx, kept_handle, "renamed",
                              "§2.2.1: an object store handle's name is initialized to the name of the "
                              "associated object store when the handle is created");
    /* §2.2.1's uniqueness, over the key the set is actually scoped to: "there must be only one object store "
       handle associated with a particular object STORE within a transaction". */
    found = idb_transaction_handle_find(ctx, tx, store);
    CHECK(JS_VALUE_GET_PTR(found) == JS_VALUE_GET_PTR(kept_handle),
          "§2.2.1: a transaction's handle set must answer with the handle already associated with that store, "
          "which is what stops §4.10's objectStore() minting a second one for a store an upgrade renamed");
    JS_FreeValue(ctx, found);

    /* The store the transaction CREATES, and then renames — the arm step 5.1 must NOT run for. */
    made = idb_object_store_create(ctx, tx, db, "u", JS_NULL, false);
    idb_object_store_rename(ctx, tx, db, made, "u2");
    made_handle = idb_object_store_handle(ctx, made, tx);
    idb_transaction_handle_add(ctx, tx, made_handle);

    /* §5.5 step 2, then §5.5 step 3. */
    idb_database_revert_transaction(ctx, tx);
    idb_abort_upgrade_transaction(ctx, tx);

    CHECK(idb_connection_version(ctx, conn) == idb_database_version(ctx, db),
          "§5.8 step 3 did not put the CONNECTION's version back to the database's — §2.1.1: the connection's "
          "version \"remains constant for the lifetime of the connection unless an upgrade is aborted, in "
          "which case it is set to the previous version of the database\"");
    CHECK(idb_connection_version(ctx, conn) == 0,
          "§5.8 step 3's second arm: \"or 0 (zero) if database was newly created\". §2.1 creates a database at "
          "0 and only §5.7 step 8 moves it, so after §5.5 step 2's revert the database IS at the number both "
          "of the step's arms name — a fixture reading anything else means the two have stopped agreeing and "
          "the arms need telling apart after all");
    idb_selftest_handle_named(ctx, kept_handle, "s",
                              "§5.8 step 5.1: a handle whose object store was NOT newly created during the "
                              "transaction takes its store's name back — which is what makes `store.name` "
                              "answer the old name after an upgrade transaction renamed the store and aborted");
    idb_selftest_handle_named(ctx, made_handle, "u2",
                              "§5.8 step 5.1's GUARD: a handle whose object store WAS newly created during the "
                              "transaction is left alone. The store it names has just been destroyed by the "
                              "revert and has no earlier name for the handle to go back to, so a step that ran "
                              "unconditionally would rename this handle to a name nothing ever answered to");
    CHECK(idb_object_store_is_deleted(ctx, made),
          "§5.5 step 2 left a store the transaction created undeleted, so §5.8 ran against a database that had "
          "not been put back");

    JS_FreeValue(ctx, made_handle);
    JS_FreeValue(ctx, kept_handle);
    JS_FreeValue(ctx, made);
    idb_selftest_finish(ctx, tx);
}

static void idb_store_selftest(JSContext *ctx)
{
    JSValue db, found, store, conn, tx;

    /* §2.1: "When a database is first created, its version is 0", and a storage key has ONE database per name
       — the set answers with the database that was created and not with a copy of it. */
    db = idb_database_create(ctx, "selftest");
    CHECK(idb_database_version(ctx, db) == 0, "§2.1: a database is first created with version 0");
    found = idb_database_find(ctx, "selftest");
    CHECK(JS_VALUE_GET_PTR(found) == JS_VALUE_GET_PTR(db),
          "§2.1: a storage key's set of databases must answer with the database created under that name");
    JS_FreeValue(ctx, found);
    found = idb_database_find(ctx, "never-created");
    CHECK(JS_IsNull(found), "§2.1: a name this storage key holds no database for must answer with none");
    JS_FreeValue(ctx, found);

    /* §2.1.1's CONNECTION and §2.7's TRANSACTION, which §5.1 and §5.7 build for a page and which this fixture
       builds for itself: §2.7's first sentence makes a transaction the only way data is written, and every
       algorithm below takes it because a change that names no transaction is a change §5.5 step 2 could never
       undo. An UPGRADE transaction, because creating an object store is what §2.7 gives only that mode. */
    conn = idb_connection_open(ctx, db);
    tx = idb_selftest_tx(ctx, conn, JS_UNDEFINED, IDB_TX_VERSIONCHANGE);

    /* §2.2: out-of-line keys, no key generator. What a record filed INTO it does is the document's assertion —
       see the block above idb_selftest_int for why §6.1 has no C entry to reach from here. */
    store = idb_object_store_create(ctx, tx, db, "s", JS_NULL, false);
    found = idb_object_store_find(ctx, db, "s");
    CHECK(JS_VALUE_GET_PTR(found) == JS_VALUE_GET_PTR(store),
          "§2.2: an object store's name is unique within its database and names that store");
    JS_FreeValue(ctx, found);

    /* The upgrade transaction ends the way §5.4's commit task ends it, so the store above is the state the
       next transaction finds — which is what §5.5 step 2 has to put back. */
    idb_selftest_finish(ctx, tx);
    idb_key_path_selftest(ctx, conn, db);
    idb_extract_selftest(ctx);
    idb_revert_selftest(ctx, conn, db, store);
    /* §5.5 step 3 runs on the state step 2 has just put back, so §5.8's fixture runs on the state this one
       left — the database at the 0 it was created with, and the store back under the name it had. */
    idb_upgrade_abort_selftest(ctx, conn, db, store);

    JS_FreeValue(ctx, conn);
    JS_FreeValue(ctx, store);
    JS_FreeValue(ctx, db);
}

/* WHICH DOCUMENT THIS RUN DRIVES, asked of the ARGUMENT VECTOR because that is the only channel a host has to
   this program. It was `getenv("APICLIENT_ASAN_MIN")`, and emscripten's `ENV` is a fixed default set that never
   merges the launching process's environment — so the minimal document, the `min` probe subset and every
   statement only that document carries were unreachable in EVERY mode of `node engine/build.mjs`. An unrunnable
   fixture is the same defect as a corpus file the collector does not collect (CLAUDE.md, Testing): it looks like
   coverage and is not. argv IS delivered (emcc's runtime hands node's trailing arguments to main), so the
   selection is made where the host can actually make it. */
static int arg_has(int argc, char **argv, const char *flag) {
    int i;
    for (i = 1; i < argc; i++)
        if (argv[i] && !strcmp(argv[i], flag)) return 1;
    return 0;
}

/* …AND A FLAG THAT CARRIES A VALUE, for the one thing this host has to be told rather than asked: WHERE its
   cold tier is. A flag with a missing operand is a path the fixture would then invent, so it says so instead. */
static const char *arg_val(int argc, char **argv, const char *flag) {
    int i;
    for (i = 1; i < argc; i++)
        if (argv[i] && !strcmp(argv[i], flag)) {
            DCHECK(i + 1 < argc && argv[i + 1] != NULL,
                   "a fixture flag that names a file was given without one — the cold tier's store is this "
                   "host's IndexedDB and a run that invents its path writes a residue nothing will read back");
            return argv[i + 1];
        }
    return NULL;
}

/* ─── THIS HOST'S COLD-TIER STORE ────────────────────────────────────────────────────────────────────────────
 * THE PARK DOCUMENT HAS TO OUTLIVE THE PROCESS, WHICH IS THE WHOLE OF WHAT A SECOND SESSION IS. `_park` rides
 * the result document to a trusted zone that puts it in IndexedDB and hands it back to qjs_begin on the next
 * visit; this fixture had no such zone, so its residue lived in `g_park` and died with the runtime — and a
 * document that dies with the writer cannot be read by anybody, which is the mechanical reason the read half
 * had never executed here. A file IS that store: the same string, the same key (the caller's path), the same
 * hand-back. What crosses is TEXT and it carries its type, which is the property SECURITY.md states about
 * every other tier boundary and is true of this one for free — cold_park_recipes is already the language
 * cold_resume parses.
 * IT IS NOT A SECOND SCHEDULER, A REPLAY HARNESS OR A DRIVER. The resume is engine_sched_begin's own choice
 * between a residue and a boot flow; all this supplies is the two sessions and the shelf between them. */
static void tf_park_store(const char *path, const char *recipes) {
    FILE *f = fopen(path, "wb");
    size_t n = strlen(recipes);
    size_t put;
    int closed;

    CHECK(f, "this host could not open its cold-tier store to write the parked residue — every flow the park "
             "just wrote down is the only remaining copy of that timeline, and it is about to go with the "
             "instance");
    put = fwrite(recipes, 1, n, f);
    closed = fclose(f);
    CHECK(put == n && closed == 0,
          "this host could not write the whole parked residue — a truncated park document is worse than none: "
          "the records it kept name segments the records it lost were standing on");
}

/* …AND BACK. The residue is handed to engine_sched_begin exactly as read: no join, no split, no re-encode —
   the one translation the extension performs (an array joined by ';') exists because IndexedDB stores JSON,
   and a host with a file has no such boundary to translate across. */
static char *tf_park_load(const char *path) {
    FILE *f = fopen(path, "rb");
    long n;
    char *buf;
    int seeked, rewound, closed;
    size_t got;

    CHECK(f, "this host could not open its cold-tier store to resume — a session asked to continue a residue "
             "and the document naming it is not there, so it would silently re-explore from the baseline and "
             "report that as a resume");
    /* THE CALLS ARE MADE, THEN ASSERTED. A CHECK's condition is evaluated for its truth and nothing else — an
       `fseek` inside one is work that a release build's compiled-out twin would have to keep, which is exactly
       the asymmetry check.h's contract forbids. */
    seeked = fseek(f, 0, SEEK_END);
    n = ftell(f);
    rewound = fseek(f, 0, SEEK_SET);
    CHECK(seeked == 0 && n >= 0 && rewound == 0,
          "this host could not size the parked residue it is about to resume");
    buf = malloc((size_t)n + 1);
    CHECK(buf, "this host could not hold the parked residue it is about to resume");
    got = fread(buf, 1, (size_t)n, f);
    closed = fclose(f);
    CHECK(got == (size_t)n && closed == 0,
          "this host read less of the parked residue than the store holds — a park document truncated on the "
          "way IN rebuilds flows standing on segments the tail of the document was going to write");
    buf[n] = 0;
    DCHECK(n > 0, "the cold-tier store holds an EMPTY residue — an empty park document is how a fully-explored "
                  "document deletes its entry, so the session that wrote this one drained instead of parking "
                  "and there is nothing here to resume");
    return buf;
}

/* ─── THE PROBE TABLE, AND THE RUN'S COMPLETION MOMENT ────────────────────────────────────────────────────
 * THE VERDICT AND THE MOMENT ARE ONE FUNCTION OF ONE STRING, which is why the table lives here rather than at
 * the end of main. The fixture's job is to DEMONSTRATE its document, so it is finished when every statement
 * that document makes has been answered in the result document — a statement about EMITTED OUTPUT, which is the
 * only kind §NO BOUNDS permits. It is not a step, time, flow or no-progress count, and it does not claim the
 * frontier was finite: this document's opaque-length walk makes every length a world, so the residue that
 * remains when the moment arrives is real and unbounded, and the run reports over what it has.
 * WITHOUT IT NO NATIVE RUN OF THIS FIXTURE HAD EVER FINISHED. Both documents' frontiers are unbounded, the
 * harness's completion condition was that they DRAIN, and so `node engine/build.mjs native min` — described in
 * build.mjs as the fast per-change memory gate — was killed by its own backstop at fifteen minutes with the
 * probe table never printed once. Every row below had therefore never been observed by anybody.
 *
 * WHICH DOCUMENT CARRIES A PROBE IS READ OFF THE DOCUMENT, never declared beside it, which is where it went
 * wrong. The field was a hand-written `docs` value (FULL / BOTH / MIN) and the table's own rule was that "a
 * probe whose statement is NOT in that document must be 0 here, or the gate asserts a fact about a program that
 * never ran". SIX ROWS BROKE IT: `s-evalc` (`state.note`), `json-fork`, `xdoc-read`, `xdoc-job`, `timer-order`
 * and `iframe-nav` were declared as both documents' while their statements are in the full one alone — so the
 * minimal gate was UNSATISFIABLE by construction, which no amount of running it could have revealed while it
 * could not finish. A hand-kept copy of a fact the document already states is the defect; the fix is to stop
 * keeping the copy.
 * SO A ROW CARRIES THE `key` OF ITS OWN STATEMENT — a substring of the document TEXT that the statement the
 * probe is about is what puts there — and membership is `strstr(doc, key)`. The SESSION is the other half and is
 * not derivable from the document: --cold-park and --cold-resume run the same one and make different statements
 * about it (what a park WROTE / what a resume REBUILT). */
/* AND A FOLDED ROW CARRIES THE NAME OF THE STATEMENT THAT IS 0 IN IT, which is the same rule one layer down.
   `node-algo` is ONE row computed from a hundred independent assertions, so its 0 says "one of a hundred" and
   that is not a localisation — it is the count-with-no-name-in-it CLAUDE.md §C-stack forbids, wearing a
   measurement's clothes instead of an engine's. It cost a real prediction this session: a report predicted a
   probe called `ce-async` green, and there is no such row — the assertion it named (`/api/ceasync` carrying
   `ceAWAIT`, the custom-element reaction that parks at a loop and at an await) is one of NODE_ALGOS' 98 rows,
   has no name of its own, and was therefore never asked as itself. A row that cannot be named cannot be
   predicted against, cannot be cited when it goes red, and cannot be reported verbatim.
   NULL for every row that is its own statement (the omitted trailing initialiser), because those already name
   themselves — `iframe-nav` is `/api/iframenav` carrying `ifnav` and nothing else, so its 0 is already a
   localisation. */
typedef struct { const char *name; int ok; const char *key; unsigned char sess; const char *why; } Probe;
enum { SESS_EXPLORE = 0, SESS_PARK = 1, SESS_RESUME = 2 };

/* THE FOLD, WITH THE NAME KEPT. `ok` is the assertion, `what` is what it is about, and the FIRST failure is the
   one kept: a fold is a conjunction, so the first 0 is where the run stopped being right and everything after
   it is unconditioned on that. `why` is a LOCAL of the caller and not a static, because probes_eval runs at
   every sample of a live run — a static would pin the first sample's failure forever, and report a statement
   that had since become true. */
static void fold_row(int *row, const char **why, int ok, const char *what) {
    if (ok) return;
    *row = 0;
    if (!*why) *why = what;
}
/* THE CALLER'S ROOM FOR THE SELECTED ROWS — a buffer size with an abort behind it (probes_eval's `n < cap`),
   never a limit on how many statements a document may make. Raised with the @S stage rows: the table is 107
   and the full document selects nearly all of them, so the old 128 left a margin the next lane to add a row
   would have spent without meaning to. */
#define PROBE_MAX 192

/* WHAT THIS INVOCATION IS: the document whose statements are being answered, which of the three sessions it is,
   and the realm the result document is rendered from. Set once in main before the scheduler runs, because the
   streamed report reads them from INSIDE the run. */
static const char *g_doc;
static int g_sess = SESS_EXPLORE;
static JSContext *g_probe_ctx;
/* …and the two censuses the cold rows are statements about, filled by main after its run. An exploring session
   leaves them at zero and selects none of those rows. */
static ColdParked g_cp;
static ColdResumed g_cr;

/* ─── ONE PARAM OF ONE ENDPOINT, READ OFF endpoint_json_array's OWN SHAPE ──────────────────────────────────
 *
 * A `strstr` over the whole result document cannot state a COUNT, and it cannot state WHICH endpoint carries
 * the byte it found, so a row written with one is a term that cannot fail. That is the objection these helpers
 * were written for, and THE MIRROR OF IT PUT THREE ROWS AT 0 FOR EVERY RUN THERE HAS EVER BEEN: a term that
 * cannot PASS. `hdr-seq`, `hdr-record` and `mp-escape` each spelled the assertion by hand as
 *     strstr(js, "\"bad\",\"validValues\":[\"threw\"]")
 * and endpoint_json_array writes `{"name":"bad","location":"query","validValues":["threw"]}` — the LOCATION
 * field sits between the two halves, so those six clauses matched nothing that file has ever emitted. Two of
 * the three rows name Web IDL conversions (§3.2.21 sequence<T>, §3.2.23 record<K, V>) that this engine builds
 * and drives (core/idl_iter.c, core/fetch/headers.c), so the 0 was read as an unbuilt capability and is not
 * about the engine at all.
 *
 * SO THE LAYOUT IS KNOWN IN EXACTLY ONE PLACE — here — AND THIS ASSERTS THAT IT STILL IS. A probe term that
 * silently stops matching is invisible: it reads 0, which is the same 0 as a real gap. The DCHECK below tells
 * the two apart at the origin, because a param whose `"name"` is present WITHOUT the `,"location":` that must
 * follow it is the emitter having moved under this reader and nothing else. */

/* ONE ENTRY of a validValues array, quote-aware — an escaped quote is a value's byte, not the entry's end, and
   a `]` inside a value is not the array's end either. `v` is a position inside the array; returns the position
   past the entry with [*pb, *pb + *pn) its raw JSON bytes, or NULL at the closing bracket. */
static const char *param_value_next(const char *v, const char **pb, size_t *pn) {
    const char *b;

    while (*v == ',' || *v == ' ') v++;
    if (*v != '"') return NULL;          /* the ']' — or a shape endpoint_json_array does not write */
    b = ++v;
    for (; *v && *v != '"'; v++)
        if (*v == '\\' && v[1]) v++;
    if (*v != '"') return NULL;
    *pb = b;
    *pn = (size_t)(v - b);
    return v + 1;
}

/* The first byte INSIDE one param's validValues array, or NULL when this endpoint or this param is absent. */
static const char *param_values_span(const char *js, const char *url, const char *pname) {
    char pat[160];
    const char *e, *end, *p, *v;

    snprintf(pat, sizeof pat, "\"url\":\"%s\"", url);
    e = strstr(js, pat);
    if (!e) return NULL;
    end = strstr(e + 1, "\"url\":\"");   /* the NEXT endpoint's url is this object's far edge */
    snprintf(pat, sizeof pat, "\"name\":\"%s\",\"location\":", pname);
    p = strstr(e, pat);
    if (!p || (end && p >= end)) {
        snprintf(pat, sizeof pat, "\"name\":\"%s\"", pname);
        p = strstr(e, pat);
        DCHECK(!p || (end && p >= end),
               "endpoint_json_array writes a param object this reader cannot parse: the `\"name\"` is there and "
               "the `,\"location\":` that must follow it is not, so every probe term over this param's values "
               "reads 0 for a reason that is not about the engine");
        return NULL;
    }
    v = strstr(p, "\"validValues\":[");
    DCHECK(v != NULL && (!end || v < end),
           "a param object carried a name and a location and no validValues array — endpoint_json_array emits "
           "all three of them unconditionally");
    if (!v || (end && v >= end)) return NULL;
    return v + strlen("\"validValues\":[");
}

/* HOW MANY VALUES ONE PARAM OF ONE ENDPOINT CARRIES — for a row whose claim is about one ("this param carries
   more than one value, therefore the walk forked instead of deciding a bound"). 0 = no such endpoint, or no
   such param on it. */
static int param_value_count(const char *js, const char *url, const char *pname) {
    const char *v = param_values_span(js, url, pname), *b;
    size_t n;
    int k = 0;

    while (v && (v = param_value_next(v, &b, &n)) != NULL) k++;
    return k;
}

/* DOES ONE PARAM OF ONE ENDPOINT CARRY A VALUE CONTAINING `needle` — for a row whose claim is about what a
   value IS rather than how many there are (here, that the record read back out of an object store is still the
   CONCOLIC that went in). Scoped to the endpoint AND to one entry: the taint rendering appears in this document
   from the XSS statements as well, so an unscoped strstr is the term that cannot fail. */
static int param_value_has(const char *js, const char *url, const char *pname, const char *needle) {
    const char *v = param_values_span(js, url, pname), *b;
    size_t n, m = strlen(needle), i;

    while (v && (v = param_value_next(v, &b, &n)) != NULL)
        for (i = 0; m <= n && i <= n - m; i++)
            if (memcmp(b + i, needle, m) == 0) return 1;
    return 0;
}

/* ONE PARAM OF ONE ENDPOINT CARRIES EXACTLY ONE VALUE AND IT IS `val` — the claim the three rows above were
   trying to make, and the one an `strstr` up to a literal `]` was standing in for. Exactly-one is the whole
   assertion for a statement whose value the code DETERMINED: a second entry means the flow forked where the
   fixture says it cannot, which is a finding and not a match.
   `val` is compared against the emitted JSON bytes, so a value needing an escape would never match; that is
   asserted rather than left to be discovered as a row stuck at 0. */
static int param_value_only(const char *js, const char *url, const char *pname, const char *val) {
    const char *v = param_values_span(js, url, pname), *b, *c;
    size_t n, m = strlen(val);
    int plain = 1;

    for (c = val; *c; c++)
        if (*c == '"' || *c == '\\' || (unsigned char)*c < 0x20) plain = 0;
    DCHECK(plain, "a probe's expected param value carries a byte json_buf_str escapes, so it is being compared "
                  "against a spelling the emitter never writes");
    if (!v || (v = param_value_next(v, &b, &n)) == NULL) return 0;
    if (n != m || memcmp(b, val, m) != 0) return 0;
    return param_value_next(v, &b, &n) == NULL;   /* and nothing after it */
}

/* THE STAGE AN @S SEARCH REACHED, read off the entry the report already carries. ONE boolean per sink
 * collapses four mechanisms — detection, the context probe, the derivation, the fire — so its 0 names none of
 * them, which is the defect `idb-record` and the taint row were each split for. It is not a theoretical
 * objection here: the SAME five rows read 1 over HTML_MIN and 0 over HTML at one commit, and nothing about
 * them says which stage the two documents differ at.
 *
 * NOTHING IS ADDED TO THE ENGINE TO ANSWER IT — every stage is already a fact of the emitted entry, and
 * solve.h declares exactly two entry shapes: a sink no attacker source reached has NO entry, a parked entry
 * carries `tried` (candidates seeded) and `reached` (candidates whose bytes arrived at the sink), and only a
 * fired one carries a `poc`.
 *
 * S_UNSEEN IS TWO FACTS AND THEY ARE TOLD APART, which is the whole point of splitting a row: "no attacker
 * source reached this sink" and "the report format moved under this reader" would otherwise both be 0 here,
 * and the second is not a measurement at all. The sink NAMES are the engine's own table, so a document that
 * emitted no entry for one still emitted entries — the assert is that the array this is reading is an @S
 * array at all. */
enum { S_UNSEEN = 0, S_SEEN, S_RAN, S_ARRIVED, S_FIRED };
static int s_stage(const char *js, const char *sink, const char *src) {
    static const char PARKED[] = ",\"search\":\"parked\",\"tried\":";
    static const char REACHED[] = ",\"reached\":";
    char pat[192];
    const char *e, *r;

    snprintf(pat, sizeof pat, "{\"sink\":\"%s\",\"source\":\"%s\"", sink, src);
    e = strstr(js, pat);
    if (!e) {
        DCHECK(strstr(js, "\"securitySinks\":[") != NULL,
               "an @S stage was read out of a result document that carries no @S array at all — the row would "
               "report `unseen` for every sink, which is a statement about the PAGE, and this is a statement "
               "about the REPORT: either result.c stopped composing the array or solve.h's entry shape moved");
        return S_UNSEEN;
    }
    e += strlen(pat);
    if (!strncmp(e, ",\"poc\":", 7)) return S_FIRED;
    DCHECK(!strncmp(e, PARKED, sizeof PARKED - 1),
           "an @S entry is neither of the two shapes solve.h declares — it carries no `poc` and no parked "
           "search, so the report has a third state this row would silently score as never having run");
    r = strstr(e, REACHED);
    DCHECK(r != NULL, "a parked @S entry carries no `reached` — `tried` alone cannot say whether a candidate "
                      "ever re-executed as far as the sink, which is the one thing this row exists to name");
    if (atoi(r + sizeof REACHED - 1) > 0) return S_ARRIVED;
    return atoi(e + sizeof PARKED - 1) > 0 ? S_RAN : S_SEEN;
}

/* A FIRE-VERIFIED PoC IS A FIELD OF ITS OWN RECORD, NEVER A SUBSTRING OF THE DOCUMENT — and reading it the
 * other way made the fixture's ONE passing @S row a false green, in a run that had fired nothing at all.
 *
 * THE CONTRADICTION IS IN THE SMOKE LINE ITSELF AND NEEDS NO NEW MEASUREMENT: `s-url=1` stood beside
 * `s-url-atsink=0`, and `s_stage` returns S_FIRED — which is >= S_ARRIVED — for exactly the records that carry
 * a `poc`. So `s-url-atsink=0` says that record has none, while `s-url=1` said a URL breakout was fire-verified.
 * Both rows were computed from the same bytes by the same function call, and they cannot both be true.
 * WHAT SATISFIED THE OLD ROW was three loose `strstr`s over the WHOLE @S array — the class name, the source,
 * and the payload text — and solve_json_array prints a parked search's OWN CANDIDATE LIST (`payloads`, which
 * for SINK_URL is `javascript:X9()` and `javascript:X9()//`, its two written-down vectors). So a search that
 * had never once arrived at its sink satisfied every conjunct out of its own to-do list. That is the exact
 * defect CLAUDE.md §Architecture names: a plausible datum is indistinguishable from a measurement, and this
 * one was worse than a 0 row because it was the only @S row reading green.
 * The pattern below is the record shape solve.h declares, in order, with the PoC anchored to its own key — so
 * a payload can only satisfy it from the `poc` of the (sink, source) record it belongs to. It is a PREFIX
 * match on the payload because a class with several vectors may fire any of them, and prefixing is what lets
 * the row name the vector without claiming which one. */
/* A FIELD OF ONE @S RECORD, NEVER A SUBSTRING OF THE DOCUMENT — the rule s_poc states, applied to a field that
 * is not the record's first. `sourceEncodes` and `sourceDelivers` sit at the TAIL of an entry, past a variable
 * number of counts, so no single anchored pattern reaches them from the record's head; a loose `strstr` over the
 * whole array reaches them from ANY record, which is exactly how the URL row came to be satisfied out of a
 * different search's candidate list. The record's own bounds are what makes the answer a measurement: an entry
 * runs from its `{"sink":` to the next one. */
static int s_field(const char *js, const char *sink, const char *src, const char *needle) {
    char pat[192];
    const char *e, *end, *p;
    size_t nl = strlen(needle), span;
    int k = snprintf(pat, sizeof pat, "{\"sink\":\"%s\",\"source\":\"%s\"", sink, src);

    CHECK(k > 0 && (size_t)k < sizeof pat,
          "an @S field row's record pattern did not fit its buffer — a truncated pattern matches a PREFIX of "
          "the record key, so the row would read a field out of some other sink's entry");
    if (!(e = strstr(js, pat))) return 0;
    end = strstr(e + 1, ",{\"sink\":");
    span = end ? (size_t)(end - e) : strlen(e);
    if (span < nl) return 0;
    for (p = e; p + nl <= e + span; p++) if (!memcmp(p, needle, nl)) return 1;
    return 0;
}

static int s_poc(const char *js, const char *sink, const char *src, const char *poc) {
    char pat[384];
    int k = snprintf(pat, sizeof pat, "{\"sink\":\"%s\",\"source\":\"%s\",\"poc\":\"%s", sink, src, poc);

    CHECK(k > 0 && (size_t)k < sizeof pat,
          "an @S PoC row's expected record did not fit its buffer — a truncated pattern matches a PREFIX of "
          "the record, so the row would assert less than it names and report green for a payload it never saw");
    return strstr(js, pat) != NULL;
}

/* Fill `out` with the rows this invocation carries and answer how many. Every row's `ok` is computed here, so
   the mid-run report and the final one are the same function of the same bytes. */
static int probes_eval(const char *js, Probe *out, int cap) {
    int has_uid_param = strstr(js, "\"/api/u\"") && strstr(js, "\"uid\"") && strstr(js, "{state}.id");
    /* THE PATH AND THE BODY, which this surface could not name at all. The address is RE-SPELLED so its hole
       is one the popup's `/\{([^}\/]+)\}/` substitution can find — `{state}.id` has its braces around the root
       source and the member path OUTSIDE them, so the segment becomes `{state.id}` — and the param is that
       name at location "path" with NO value, because a symbolic server-injected field determined none. */
    int path_param = strstr(js, "\"/v1/users/{state.id}/posts\"") && strstr(js, "\"name\":\"state.id\"") &&
                     strstr(js, "\"location\":\"path\"");
    /* THE BODY'S FIELDS, from the bytes the page composed, with the literals the code computed. */
    int body_param = strstr(js, "\"name\":\"title\"") && strstr(js, "\"firstPost\"") &&
                     strstr(js, "\"name\":\"count\"") && strstr(js, "\"location\":\"body\"");
    /* THE ALIGNED EXAMPLE: the shape's hole and the concolic's concrete URL line up segment for segment, so the
       path param carries `visible` — the value the document actually has. The query param on the same record is
       what proves all three locations coexist rather than one overwriting the others. */
    int path_example = strstr(js, "\"/v1/vis/{hidden|visible}/reports\"") &&
                       strstr(js, "\"name\":\"hidden|visible\"") && strstr(js, "\"visible\"") &&
                       strstr(js, "\"name\":\"deep\"");
    int role_admin = strstr(js, "admin") != NULL;
    int role_public = strstr(js, "public") != NULL;
    int data_count = 0; for (const char *p = js; (p = strstr(p, "\"/api/data\"")); p++) data_count++;
    int merged = (data_count == 1);   /* /api/data appears ONCE with role=[admin,public] merged across flows */

    int pinned = strstr(js, "/api/region/us-east-1") != NULL;   /* EQ gate concretized region to the REAL value */
    int lazy = strstr(js, "/api/admin/audit-log") != NULL;   /* endpoint reachable ONLY via the admin-arm lazy chunk */

    /* DOM TIME-TRAVEL: the two flows forked at if(cfg.admin) wrote 'data-tt' on the SHARED <body> to DIFFERENT
       values, then AFTER the fork each read it BACK into /api/whoami?tt=. Both ttADMIN and ttPUBLIC present ⇒
       each flow saw ITS OWN document (per-flow DOM delta swapped on context-switch). If DOM isolation were
       broken, one flow would read the other's write and only ONE value would survive. */
    int dom_admin  = strstr(js, "ttADMIN") != NULL;
    int dom_public = strstr(js, "ttPUBLIC") != NULL;
    int dom_attr = (strstr(js, "\"/api/whoami\"") && dom_admin && dom_public);
    /* NODE-INSERT time-travel: each flow appended its OWN child; both marks present ⇒ neither flow's inserted
       node leaked into the other's tree (the kind-1 insert delta reverts per-flow). */
    int dom_node = (strstr(js, "\"/api/kid\"") && strstr(js, "kidADMIN") && strstr(js, "kidPUBLIC"));
    int dom_tt = dom_attr && dom_node;
    /* ACCESSOR time-travel: each flow assigned rx.flag (an accessor) -> its setter wrote rx._f, isolated per
       flow; reading rx.flag back gives its OWN value. Both flagADMIN and flagPUBLIC present ⇒ the accessor is
       skipped by capture (only the backing _f slot reverts) — no setter re-invocation corruption. */
    int accessor_tt = (strstr(js, "\"/api/flag\"") && strstr(js, "flagADMIN") && strstr(js, "flagPUBLIC"));
    /* ASYNC-AS-FLOW: each flow's promise .then reaction fired under its OWN COW -> both thenADMIN and thenPUBLIC
       present ⇒ microtask reactions run as first-class per-flow flows (not a dropped/global-drained job). */
    int async_tt = (strstr(js, "\"/api/then\"") && strstr(js, "thenADMIN") && strstr(js, "thenPUBLIC"));
    /* AWAIT + FORK: the ternary (cfg.admin) is a concolic branch INSIDE the nested async function, so it forks
       via a deep SNAPSHOT fork of the async body's live tramp chain (clone_deep_flow: the async frame's state is
       cloned from async_data->func_state.frame with a FRESH promise capability — never a re-run). Both awADMIN and
       awPUBLIC present ⇒ await suspend/resume works AND the deep async-branch fork is correct per flow. */
    int await_tt = (strstr(js, "\"/api/await\"") && strstr(js, "awADMIN") && strstr(js, "awPUBLIC"));
    /* ASYNC-AS-CALLER deep fork: the concolic branch is in a HELPER called by the async body (chain base->async->
       helper), so the async frame is a mid-chain CALLER whose func_state.frame is the deeper frame's caller buffer.
       Both acADMIN and acPUBLIC ⇒ clone_deep_flow sourced the async caller's stack correctly (tramp_buf_base). */
    int asynccall_tt = (strstr(js, "\"/api/asynccall\"") && strstr(js, "acADMIN") && strstr(js, "acPUBLIC"));
    /* ASYNC THROW: a heap-resident async body that throws becomes a REJECTED promise; the .catch reaction fires
       and records /api/caught — proving the exception unwind of an async tramp frame (reject, not propagate). */
    int async_throw = (strstr(js, "\"/api/caught\"") && strstr(js, "asyncThrew"));
    /* ASYNC LOOP + PREEMPT: an async body with a loop is preempted mid-iteration; the deep-preempt stashes and
       rebuilds the whole TrampFrame chain (incl. async_data), so it resumes correctly. s=0+..+24=300. */
    int async_preempt = (strstr(js, "\"/api/asyncloop\"") && strstr(js, "\"2000\""));
    /* ORPHAN-INVOKE, IN TWO ROWS BECAUSE IT IS TWO CLAIMS. The first is that a function nothing in the document
       calls was RUN — with no driving there is no request at all, which is what a page holding only such a
       function measured. The second is that its parameter arrived as unknown external input: `role === 'admin'`
       is an equality over it, so a value the engine had invented (`undefined`, or any concrete pick) decides
       the branch and the arm behind it is never explored, while an unknown FORKS and both arms run. A single
       row could not tell "the orphan ran" from "the orphan ran with a fabricated argument". */
    int orphan_driven = strstr(js, "\"/api/orphan/report\"") != NULL;
    int orphan_gate   = strstr(js, "\"/api/orphan/admin-only\"") != NULL;
    /* AND THE THIRD CLAIM: the loop INSIDE a driven orphan suspended and resumed. The number is computed by the
       body across 3000 back-edge preempts, so the exact path is the whole assertion — a resume that dropped,
       reordered or restarted an iteration lands on a different number and this row goes red with the wrong
       value visible, rather than on a timeout with nothing in it. */
    int orphan_loop   = strstr(js, "\"/api/orphan/loop3000\"") != NULL;
    /* AND THE FOURTH: the driven body compared its unknown against an OBJECT and survived it. Nothing about
       the request is interesting — reaching it at all is the claim, because spelling that operand aborted the
       instance and an abort emits NO endpoints, so this row and every row above it went dark together. */
    int orphan_ident  = strstr(js, "\"/api/orphan/identity\"") != NULL;
    /* AND THE FIFTH: the driven body put its unknown through a coerce-then-compute builtin and survived it.
       Same shape as the row above — the request is uninteresting and reaching it is the claim — but a
       different mechanism: this one is §7.1.4 ToNumber over unknown input inside a C body, which aborted the
       instance and took every row here down with it. */
    int orphan_ccode  = strstr(js, "\"/api/orphan/charcode\"") != NULL;
    /* AND THE SIXTH AND SEVENTH, both "the driven body survived an operation over its unknown". They are
       separate rows because they are separate mechanisms and each passed while the other crashed: §13.4.3.1's
       postfix ToNumeric is an OPERATOR that answers on the interpreter's own path, while §21.3.2.26's is a
       coerce-then-compute BUILTIN whose generic body converts for itself. The request is uninteresting in both;
       reaching it is the claim. */
    int orphan_update = strstr(js, "\"/api/orphan/update\"") != NULL;
    int orphan_clamp  = strstr(js, "\"/api/orphan/clamp\"") != NULL;
    /* FETCH-AWAIT-RESULT: `await fetch('/api/config')` delivered the reply and §6.4.3 json() parsed it,
       whose .region flowed into /api/user?region=us-west-2 as a CONCRETE example — a safe GET's result driving
       API-value learning, through the Response the shipped fetch component actually hands back. */
    int fetch_await = (strstr(js, "\"/api/user\"") && strstr(js, "us-west-2"));
    /* §6.4 clone(): the copy read the body, the ORIGINAL still read it afterwards, and cloning a read body
       threw where the page put its catch. A caching layer's first move is `res.clone()`, so without it the
       reply — and every endpoint behind it — was lost at that line. */
    int clone_body = (strstr(js, "\"/api/clonebody\"") && strstr(js, "\"copy\"") &&
                      strstr(js, "\"orig\"") && strstr(js, "sync"));
    /* §6.4.1/§6.4.2: the reply read as BYTES. 22 is the fixture reply's length and 123 is its leading '{' — the
       two agreeing is what says arrayBuffer() and bytes() are two views of ONE byte sequence, and a length the
       record carried rather than one a strlen guessed. */
    int body_bytes = (strstr(js, "\"/api/bodybytes\"") && strstr(js, "\"22\"") &&
                      strstr(js, "\"123\"") && strstr(js, "truetrue"));
    /* THE LATCH TIME-TRAVELS: two arms forked BEFORE either read one shared reply, and BOTH read it. If the
       body-used flag did not ride the COW delta, the second arm's read would throw and only one tag would be
       here — which is exactly what it did, as a `body stream already read` page error. */
    int body_iso = (strstr(js, "\"/api/bodyiso\"") && strstr(js, "us-west-2-bodyADMIN") &&
                    strstr(js, "us-west-2-bodyPUBLIC"));
    /* THE REQUEST'S IDENTITY IS THE PAIR: a GET and a POST of ONE address were listed as one request and
       answered with one reply, so the POST's promise settled with the GET's. `p` is the method the fixture host
       ECHOED onto the reply the POST's flow actually received, so `POST` here is the seam delivering each flow
       the answer to its own question — and the old `p=GET` is exactly the wrong answer this probe exists for.
       The `v` prefix is what makes the assert about THIS value: a bare `POST` also appears as an endpoint
       record's own method field, so the probe would pass on a record that says nothing about which reply
       arrived. `vPOST` can only have come through the header on the POST's own reply. */
    int verb_key = (strstr(js, "\"/api/verb\"") && strstr(js, "vGET") && strstr(js, "vPOST"));
    /* §5 Headers: the record fill ran (acc), the list keeps repeats (sc=2 with both values), `get` combines
       them (join=k1, k2), `set` replaces them all (set=k3), a name is matched case-insensitively (has=truefalse)
       and an absent header is null rather than "". */
    int hdrs = (strstr(js, "\"/api/hdrs\"") && strstr(js, "application/json") &&
                strstr(js, "2:a=1:b=2") && strstr(js, "k1, k2") &&
                strstr(js, "\"k3\"") && strstr(js, "truefalse") && strstr(js, "\"null\""));
    /* The record arm through a PROXY: its ownKeys and its get ran as the page's code (seen == 'trapped') and
       the header still arrived. */
    int hdrproxy = (strstr(js, "\"/api/hdrproxy\"") && strstr(js, "\"tv\"") && strstr(js, "trapped"));
    /* §5.2's iterable<>: sorted names, values combined per name, set-cookie NOT combined, and forEach handing
       (value, key, headers) in that order. `for...of` over the Headers is entries, so e and f agree. */
    int hdriter = (strstr(js, "\"/api/hdriter\"") &&
                   strstr(js, "set-cookie|set-cookie|x-a|x-b") &&
                   strstr(js, "c1|c2|1, 9|2") &&
                   strstr(js, "set-cookie=c1;set-cookie=c2;x-a=1, 9;x-b=2;") &&
                   strstr(js, "set-cookie:c1;set-cookie:c2;x-a:1, 9;x-b:2;") &&
                   !strstr(js, "BADTHIS"));
    /* §5.1's sequence arm: a generator init (the protocol, not an array walk), a Map (iterable, not an array),
       an array, a malformed pair, and null — which is NOT "no init". */
    int hdrseq = (param_value_only(js, "/api/hdrseq", "g", "g1, g2") &&
                  param_value_only(js, "/api/hdrseq", "m", "m1") &&
                  param_value_only(js, "/api/hdrseq", "a", "a1") &&
                  param_value_only(js, "/api/hdrseq", "bad", "threw") &&
                  param_value_only(js, "/api/hdrseq", "nul", "threw"));
    /* §3.2.23's record dedup is by ByteString KEY and case-SENSITIVE, so both pairs of {'X-Rec','x-rec'} reach
       the fill and both APPEND — `get` combines them into "r1, r2" over the one name the list holds. The fill's
       old replace loop matched the lowercased HEADER name instead and answered "r2". */
    int hdrrec = (param_value_only(js, "/api/hdrrec", "v", "r1, r2") &&
                  param_value_only(js, "/api/hdrrec", "k", "x-rec"));
    /* §4.10.22.8's escape over a field name written inside quotes: `a"\r\nb` is `a%22%0D%0Ab`, and the RAW
       quote must be gone — a body that carried it would let a page's own field name close the quoted name and
       forge a part header. */
    int mpesc = (param_value_only(js, "/api/mpesc", "esc", "true") &&
                 param_value_only(js, "/api/mpesc", "raw", "false"));
    /* THE ENDPOINT CARRIES ITS HEADERS: the two literals as the strings the code computed, and the
       Authorization as a SHAPE, because a value built out of unknown input is not one this engine may invent.
       The method comes from the same init, so a POST recorded as a GET would fail here too. */
    int needsauth = (strstr(js, "\"/api/needsauth\"") && strstr(js, "\"POST\"") &&
                     strstr(js, "\"x-api-version\":\"2024-11-01\"") &&
                     strstr(js, "\"content-type\":\"application/json\"") &&
                     strstr(js, "\"authorization\":\"Bearer {state}.token\""));
    /* PENDING await: the awaited promise resolved LATER (via a microtask, standing in for the network); the flow
       parked at the await and resumed when it settled -> /api/lazy?r=lazyRegion. The real fetch-parking path. */
    int pending_await = (strstr(js, "\"/api/lazy\"") && strstr(js, "lazyRegion"));
    /* PER-FLOW PROMISE STATE: a promise created BEFORE a snapshot fork and settled per-flow with a DIFFERENT
       value in each arm; both shBETA and shSTABLE present ⇒ the shared promise's settlement is isolated per
       flow (no cross-flow contamination) — async STATE time-travels, not just async execution. */
    int promise_state = (strstr(js, "\"/api/shared\"") && strstr(js, "shBETA") && strstr(js, "shSTABLE"));
    /* DELETE ISOLATION: the gamma-true flow deletes a SHARED baseline object's key; the gamma-false flow must
       still see keepVAL. Both present ⇒ the delete time-travels. FAILS under the snapshot fork (it shares the
       flow_local object across siblings) — a real unsoundness this asserts LOUD rather than leaving unchecked. */
    int delete_iso = (strstr(js, "\"/api/tok\"") && strstr(js, "wasDeleted") && strstr(js, "keepVAL"));
    /* THE SAME ISOLATION OVER THE EXOTIC GLOBAL, which is a different code path and not a second spelling of the
       row above. A slot removal on the global misses the shape (the name is not one of the Window class's
       supported indices) and reaches the exotic [[Delete]] dispatch, which the swap must not consult: 10.5.10
       [[Delete]] step 7's trap is the page's own code and every host class's hook is a VIEW over storage that is
       captured on the object that owns it, so consulting either removes state the delta never named. gdSOLE on
       BOTH arms says each flow read back its own creation AND that neither absent-slot delete left a slot
       standing for the sibling. */
    int global_delete = (strstr(js, "\"/api/gdel\"") && strstr(js, "gdADMINgdSOLE") &&
                         strstr(js, "gdPUBLICgdSOLE"));
    /* FLOW-LOCAL post-fork isolation: an object created before a concolic fork (flow_local at creation) is SHARED
       by the snapshot; each arm's mutation must be per-flow. Both values present ⇒ cow_delta_fork's forked=1 keeps
       post-fork flow_local mutations captured — no leak (the older 'snapshot shares flow_local (unsound)' claim). */
    int floc_iso = (strstr(js, "\"/api/floc\"") && strstr(js, "flocADMIN") && strstr(js, "flocPUBLIC"));
    /* CONCOLIC FORK inside a forEach CALLBACK: the branch forks deep (base->iter-callback frame); clone_deep_flow
       clones the JSArrayEvery cont_state so the sibling continues the iteration on its own. All four combinations
       (feADMIN/fePUBLIC × elements 1,2) ⇒ both arms iterated independently over the full array. */
    int fefork_tt = (strstr(js, "\"/api/fefork\"") && strstr(js, "feADMIN") && strstr(js, "fePUBLIC")
                     && strstr(js, "1feADMIN") && strstr(js, "2feADMIN") && strstr(js, "1fePUBLIC") && strstr(js, "2fePUBLIC"));
    /* SHARED-ARRAY append isolation: each arm's push into the pre-fork array must be COW-isolated. STRICT (quoted)
       values — 'pushA' and 'pushB' must appear as COMPLETE join results; the contaminated 'pushA,pushB' would
       have NO standalone "pushB" JSON value, so this fails on any leak. */
    int pushfork_tt = (strstr(js, "\"/api/pushfork\"") && strstr(js, "\"pushA\"") && strstr(js, "\"pushB\""));
    int owfork_tt = (strstr(js, "\"/api/owfork\"") && strstr(js, "\"owA\"") && strstr(js, "\"owB\""));
    /* MAP fork: clean per-arm result arrays, no dropped element. Require the two canonical arms AND the absence
       of any leading-dash join (a dropped element0 like "-mA2"). */
    int mapfork_tt = (strstr(js, "\"/api/mapfork\"") && strstr(js, "mA1-mA2") && strstr(js, "mP1-mP2") && !strstr(js, "\"-m"));
    /* CONCOLIC FORK inside a synchronously-driven GENERATOR body: the branch forks the tramp-driven generator
       activation (the named nested-activation hold-out). Both ggADMIN and ggPUBLIC present ⇒ a branch inside a
       generator body snapshot-forks per arm, never DFAILs / drives to completion. */
    int genfork_tt = (strstr(js, "\"/api/genfork\"") && strstr(js, "ggADMIN") && strstr(js, "ggPUBLIC"));
    /* TWO-BRANCH generator fork: the dedup-REPLACE path (a sibling with a gen_data swap re-forks the same
       generator). All four gate combinations present ⇒ each fork advanced its own per-flow generator state. */
    int gen2fork_tt = (strstr(js, "\"/api/gen2fork\"") && strstr(js, "g2:AX") && strstr(js, "g2:AY") && strstr(js, "g2:PX") && strstr(js, "g2:PY"));
    /* FOR-OF generator-body fork: the generator object is recovered from the caller stack (caller_sp[forof_off]),
       not a held frame ref. Both ofA and ofP present ⇒ a branch inside a for-of-driven generator body forks per arm. */
    int genofork_tt = (strstr(js, "\"/api/genofork\"") && strstr(js, "ofA") && strstr(js, "ofP"));
    /* Array.from(GEN) consumer fork: the gen body branches while CONSUMED by Array.from on the tramp
       (CONT_ITER_CONSUME). Both afA and afP present ⇒ clone_deep_flow's gen-branch cloned the JSIterConsume state
       so each fork arm accumulated its own result independently. */
    int afromfork_tt = (strstr(js, "\"/api/afromfork\"") && strstr(js, "afA") && strstr(js, "afP"));
    /* [...GEN] spread consumer fork: the SPREAD-sink variant of CONT_ITER_CONSUME. Both spA and spP present ⇒ the
       spread consumer forked mid-consume and each arm appended to its own COW-isolated array. */
    int spreadfork_tt = (strstr(js, "\"/api/spreadfork\"") && strstr(js, "spA") && strstr(js, "spP"));
    /* SHARED-SET record isolation: a pre-fork Set, each arm adds a different record. Both sadA and sadP present ⇒
       the map_add capture isolated each arm's Set.add (JS_MapDeleteRecord on unapply, JS_MapAddRecord on apply). */
    int setaddfork_tt = (strstr(js, "\"/api/setaddfork\"") && strstr(js, "sadA") && strstr(js, "sadP"));
    /* new Set(GEN) consumer fork: the Set iterator-consumer (SET sink) forks mid-consume — now fork-safe via the
       map_add COW capture. Both seA and seP present ⇒ each arm consumed into its own COW-isolated Set. */
    int setfork_tt = (strstr(js, "\"/api/setfork\"") && strstr(js, "seA") && strstr(js, "seP"));
    /* SHARED-MAP overwrite/delete isolation: one arm overwrites a pre-fork Map key, the other deletes it. Both mmA
       and gone present ⇒ the map_mutate undo-log capture isolated each arm's mutation (restored on unapply). */
    int mapmutfork_tt = (strstr(js, "\"/api/mapmutfork\"") && strstr(js, "mmA") && strstr(js, "gone"));
    /* for-await(GEN) consumer fork: the sync gen body branches while driven by the async-from-sync consumer
       (CONT_ASYNC_FROM_SYNC). Both afsA and afsP present ⇒ clone_deep_flow cloned the JSAsyncFromSync state with a
       fresh wrapper promise per arm, so each for-await arm delivered its OWN value. */
    int afsfork_tt = (strstr(js, "\"/api/afsfork\"") && strstr(js, "afsA") && strstr(js, "afsP"));
    /* Promise.all(GEN) consumer fork at index==0: the gen branches during the FIRST .next() before any element .then
       is attached (CONT_PROMISE_ALL). Both pafA and pafP present ⇒ clone_deep_flow cloned the JSPromiseAll aggregate
       fresh per arm, so each arm's aggregate resolved with its OWN element. */
    int paffork_tt = (strstr(js, "\"/api/paffork\"") && strstr(js, "pafA") && strstr(js, "pafP"));
    /* Promise.all(GEN) consumer fork at index>0: the gen yields element 0 (p0) THEN branches. Both p0-pf2A and
       p0-pf2P present ⇒ the retained pre-fork element wrapper was RE-ATTACHED to the sibling aggregate (each arm's
       a[0]=='p0' shared, a[1] its own) — the deep async-COW re-attach path, not just the index==0 base case. */
    int paf2fork_tt = (strstr(js, "\"/api/paf2fork\"") && strstr(js, "p0-pf2A") && strstr(js, "p0-pf2P"));
    /* reduce ACCUMULATOR fork (CONT_ARRAY_REDUCE): the reducer branches on opaque state mid-fold. Both pure-path
       accumulators r:rA1rA2 and r:rP1rP2 present ⇒ clone_deep_flow cloned the JSArrayReduce state so each arm
       threaded its OWN accumulator across both elements (never a shared/contaminated acc). */
    int redfork_tt = (strstr(js, "\"/api/redfork\"") && strstr(js, "r:rA1rA2") && strstr(js, "r:rP1rP2"));
    int rerepfork_tt = (strstr(js, "\"/api/rerepfork\"") && strstr(js, "xrrA1yrrA2") && strstr(js, "xrrP1yrrP2"));
    /* MACHINE-MODE ToPrimitive fork: the JSToPrim driving the page's toString has an OUTER requester (the
       JSArrayJoin machine coercing its separator), and that machine owns no frame — it is reachable only
       through `outer`, so nothing but the requester walk clones it. Both xtpAy and xtpPy present ⇒ each arm
       finished its own join with its own separator off its own machine. */
    int toprimfork_tt = (strstr(js, "\"/api/toprimfork\"") && strstr(js, "xtpAy") && strstr(js, "xtpPy"));
    /* generator .next() driven via .call (gci.next.call(gci)): the reflection bypass that previously DFAILed as a
       drive-to-completion is now routed onto do_generator_tramp at do_forward_call. Both gcA and gcP present ⇒ the
       branch inside the .call-driven generator body snapshot-forks per arm, never drives to completion. */
    int gcallfork_tt = (strstr(js, "\"/api/gcallfork\"") && strstr(js, "gcA") && strstr(js, "gcP"));
    /* generator .next() via Function.prototype.apply and via Reflect.apply — the two apply-reflection bypasses,
       reshaped at OP_call_method to the [this=gen, next, arg0] shape and routed onto do_generator_tramp. Both arms
       of each ⇒ the apply-driven generator body snapshot-forks per arm, never drives to completion. */
    int gapplyfork_tt = (strstr(js, "\"/api/gapplyfork\"") && strstr(js, "gapA") && strstr(js, "gapP"));
    int grefapplyfork_tt = (strstr(js, "\"/api/grefapplyfork\"") && strstr(js, "graA") && strstr(js, "graP"));
    /* OPAQUE ITERATION over unknown injected state. `state.items` has no length, so LengthOfArrayLike has no
       answer and every length is a world: the walk is a CHAIN of per-position outcome forks. Two facts are
       asserted and both are the point. (1) The two element reads are INDEPENDENT SOURCES — {state}.items.0 and
       {state}.items.1 — never one source answered twice, which is what the per-POSITION key in the outcome seam
       exists to guarantee. (2) `n` carries more than one value, because a chain that forked would report the
       length of each arm; one length would mean the walk decided a bound instead of forking it. This statement
       had NO row here at all, so nothing asserted it in either document — the fixture's own rule is that a probe
       which is not declared does not exist.
       THE SECOND FACT WAS NOT BEING ASSERTED. It read `strstr(js, "\"n\"")`, which is TRUE of every run that
       emits this endpoint at all — the statement always builds `?n=`, so the term could not fail, and the row
       reported the forked walk while checking only that the walk had happened once. The count is what the claim
       is about, so the count is what is read; the endpoint's own presence is subsumed by asking for it. */
    int optiter_tt = (param_value_count(js, "/api/optiter", "n") > 1 &&
                      strstr(js, "{state}.items.0") && strstr(js, "{state}.items.1"));
    /* THE SYNCHRONOUS HOST READ resumed at its own call site, three times in a loop, each answer landing in
       the call that asked for it — hr0hr1hr2 in order, never interleaved or reused. */
    int hostreq_tt = (strstr(js, "\"/api/hostreq\"") && strstr(js, "hr0hr1hr2"));
    /* AND ACROSS A FORK: each arm re-issued its own request under its own world, so BOTH answers exist. */
    int hostreqfork_tt = (strstr(js, "\"/api/hostreqfork\"") && strstr(js, "hrA") && strstr(js, "hrP"));
    /* §7.4 returned a WindowProxy for a child document the host minted, after suspending for it. */
    int navopen_tt = (strstr(js, "\"/api/navopen\"") && strstr(js, "proxy"));
    /* §7.2.5.1: a cross-origin proxy answers `closed` and refuses `name`. */
    int sop_tt = (strstr(js, "\"/api/sop\"") && strstr(js, "SecurityError:closedok") && !strstr(js, "LEAKED"));
    /* A cross-document read suspended and resumed with the peer's answer. */
    int xdocread_tt = (strstr(js, "\"/api/xdocread\"") && strstr(js, "xread"));
    /* The same cross-document read reached from inside a QUEUED JOB, which parks the flow at a job root. */
    int xdocjob_tt = (strstr(js, "\"/api/xdocjob\"") && strstr(js, "jobread"));
    /* §8.6 + §8.1.7: the microtask ran first, then the two timers in the order they were set. */
    int timer_tt = (strstr(js, "\"/api/timerfire\"") && strstr(js, "ABC"));
    /* §8.7's string handler was compiled and RUN as a program, and the handle the two arms hand back comes
       out of one per-global counter. Two endpoints because the two halves fail independently. */
    int timerstr_tt = (strstr(js, "\"/api/timerstr\"") && strstr(js, "strran"));
    int timerhandle_tt = (strstr(js, "\"/api/timerhandle\"") && strstr(js, "next") &&
                          !strstr(js, "reused"));
    /* §8.7 step 4 asked about an unknown `timeout`: the fork has a consumer, so the document survives it.
       A 0 here is an ABORT at the fork seam and not a missing endpoint — see the statement that produces it. */
    int unkdelay_tt = (strstr(js, "\"/api/unkdelay\"") && strstr(js, "forked"));
    /* §4.8.5: an inserted iframe got a child navigable, its proxy is STABLE across reads, and a read through it
       resolved to the peer's answer. */
    int ifnav_tt = (strstr(js, "\"/api/iframenav\"") && strstr(js, "ifnav"));
    /* §5.1's open through to `success`, §2.7 + §2.8's request inside it, and §2.5's LIST key path over the
       store the same upgrade creates — the marker's own sentence-by-sentence account is beside the statement
       that builds it.
       THIS ROW READ AN ENDPOINT NOTHING EMITS. It was written for the `idbTransaction`/`idbGet` host edges the
       fixture used before `indexedDB.open` existed, and the commit that replaced them with the page's own door
       deleted both the edges and the `/api/idbreq` they fetched — and left this line reading it, and reading a
       `readonly:` marker the upgrade transaction never produced. So it has been 0 for every full-document run
       since, which is CLAUDE.md's greppable tell exactly: a name READ in one place and WRITTEN in none. The
       whole marker is named here rather than a fragment of it, because a fragment is what let the first half
       of this string go on looking right after the second half stopped existing. */
    int idbopen_tt = (strstr(js, "\"/api/idbopen\"") &&
                     strstr(js, "0:1:versionchange:pending:a+b:same:plain:DataError:99:done:7:u-9"
                                ":a+b:perhandle:1:fixture"));

    /* §6.1 AND §5.5 STEP 2 OVER RECORDS, from the page's own door — the four claims the in-C round-trip
       fixture used to make before §6.1 became a step machine with no C entry. The taint term is read as the
       CONCOLIC's own rendering in the emitted URL: a store that de-tainted would emit the example instead,
       which is the one difference between a fork the solver keeps and one it silently loses. */
    /* §2.6's INDEX end to end: §4.5's createIndex (both arms of §2.6's multiEntry flag), §4.6's attributes and
       its one-handle-per-index note, §6.1 step 5's two write arms, §6.3's two retrievals and §6.5 over an
       index source. `multi2` is the multiEntry key doing the thing it exists for — one record per subkey, so
       both records answer for tag 'b' — which was dead code until §6.1 step 5 was built.
       `popben` and `back2` are §4.5's NOTE: an index created over a store that ALREADY HOLDS RECORDS, whose
       entries exist only because the population request §4.5 calls "an asynchronous request within the upgrade
       transaction" walked them. Both would read empty if createIndex populated nothing. */
    int idbidx_tt = strstr(js, "by_name:name:nu:me:own:same:refbob:pk2:popben:back2:multi2") != NULL;

    /* THE OTHER HALF OF THAT NOTE — §4.5's worked example, where the population's ConstraintError aborts the
       whole UPGRADE TRANSACTION rather than failing one `put`. Two errors, from two objects, and neither is
       derivable from the other: the transaction's is why it died, the open request's is §5.1 step 10.8's. */
    int idbuniq_tt = (strstr(js, "\"/api/idbuniq\"") && strstr(js, "ConstraintError:AbortError"));

    /* §6.1's TWO CLAIMS, as the tokens the page's own comparison emitted. The expected bytes are in the
       document (see `/api/cssprop` in HTML above) because that is where the read happens and where nothing
       percent-encodes them; what reaches here is a verdict, and its failing spelling carries the actual value
       so a red row names what answered instead of only that something did. */
    int cssprop_tt = strstr(js, "CSSPROPOK") != NULL;
    int csspropText_tt = strstr(js, "CSSTEXTOK") != NULL;

    /* CSS NESTING'S THREE CLAIMS, as the tokens the page's own comparisons emitted — the same arrangement the
       two rows above have and for the same reason: §6 "CSSOM"'s absolutized `selectorText`, §4's `:is()`
       specificity carried through the resolved cascade, and §3.3's nested group rule are three capabilities,
       and one boolean over them would say only that one of three unrelated things is wrong. The failing
       spelling of each carries the value that answered instead, so a red row names what to fix. */
    int cssnestsel_tt = strstr(js, "NESTSELOK") != NULL;
    int cssnest_tt = strstr(js, "NESTOK") != NULL;
    int cssnestmedia_tt = strstr(js, "NESTMEDIAOK") != NULL;

    /* TWO ROWS BECAUSE THESE ARE TWO INDEPENDENT CLAIMS, and one boolean over both is not a measurement of
       either — the same reason `deepest` and `completed` are two numbers a few hundred lines up. This was ONE
       row, it has read 0 for as long as it has existed, and a 0 said only "one of two unrelated things is
       wrong": either §6.1/§5.5's record semantics, or the taint round trip, and no run could tell them apart.
       That is the shape §Offensive-programming calls a plausible datum — it has a value, and the value carries
       no information about what to fix.
       `idb-record` is the SEMANTICS chain, and it is one accumulating string in placement order, so the
       longest prefix that matches localises which stage stopped: §6.1 step 2's ConstraintError on `add`, §2.2's
       key ordering through §6.2, §6.1 step 3's overwrite, the two writes read back, then §5.5 step 2's revert
       of both across a real `abort()`.
       `idb-record-taint` is the half a browser's own suite cannot have: `location.hash` goes into a store and
       has to come back out STILL CONCOLIC, because a page keeps its session in here and reads it into gated
       code. It is a claim about §5.11's clone and §6.1 step 4's serialization, not about transactions at all —
       a store that de-tainted on the way in would emit the EXAMPLE and read as a perfectly healthy endpoint. */
    int idbrec_tt = strstr(js, "ConstraintError:first1:over21:made77gone:undoneAD") != NULL;
    int idbtaint_tt = param_value_has(js, "/api/idbrec", "t", LOCATION_HASH_SHAPE);
    /* THE CONTROL, read off the same endpoint record: the same source, the same param list, no store in
       between. A 0 on BOTH means the loss is upstream of Indexed Database and a 0 on `t` alone means §5.11's
       clone or §6.1 step 4's serialization is where the triple collapses to its example.
       AND THE SHAPE IS THE COMPONENT'S OWN TOKEN, WHICH IS WHY BOTH ROWS READ 0 FOR EVERY RUN THEY HAVE
       EXISTED FOR. They asked for `{hash}` — a spelling nothing in this tree writes. It was the shape of the
       in-C `concolic_new(ctx, "{hash}", "{hash}", …)` these two rows were first written over, and when the
       fixture became the page's own `location.hash` the source's real shape (`{location.hash}`, and it is
       core/frame/location.h that spells it) was never carried across. So the control's 0 did not localise the
       loss upstream of the store: there was no loss. `location.hash` reached the @H param carrying its domain
       the whole time, and a row asserting a name no producer writes reports a healthy mechanism as broken
       forever — which is exactly the defect `idb-open` above is annotated for, one file away. Bound to the
       macro now: a probe cannot ask about a spelling the component does not have. */
    int lochash_tt = param_value_has(js, "/api/idbrec", "h", LOCATION_HASH_SHAPE);

    /* THE NAVIGATOR GATES. A UA sniff and a touch check are where a real bundle hides its other endpoints, and
       both are exactly the shape that would be LOST if the member were bare-concrete: the example decides one
       arm and the sibling is never reached. Both arms present is the whole claim. */
    int uafork_tt = (strstr(js, "\"/api/uafork\"") && strstr(js, "chrome") && strstr(js, "other"));
    int touchfork_tt = (strstr(js, "\"/api/touch\"") && strstr(js, "touch") && strstr(js, "mouse"));
    int layoutfork_tt = (strstr(js, "\"/api/layout\"") && strstr(js, "mobile") && strstr(js, "desktop"));
    /* §2.7's FLATTENING OVER AN UNKNOWN OPTIONS BAG, and it is the SAME claim one level below the union arm:
       Web IDL §3.2.25 picks the arm, DOM §2.7's flatten options step 2 reads `capture` off it, and the value
       it reads is another unknown — so ONE registration is a capturing listener AND a bubbling one, which the
       dispatch distinguishes by the phase each runs at. Both phases present is the whole claim; either alone
       is a ToBoolean pinning the flag at the boundary, which is what the plain reader did and which made the
       arm fork above it half-useless. */
    int aelunk_tt = (strstr(js, "\"/api/aelunk\"")
                     && strstr(js, "aelcapturing") && strstr(js, "aelbubbling"));
    /* THE BITWISE/SHIFT FAMILY OVER UNKNOWN INPUT. Both arms present is the whole claim: a `&` whose result
       collapsed to a bare number (or to NaN) decides the branch and the sibling is never reached, which is
       precisely the coverage §Re-execution means by "opacity SURVIVES numeric coercion". `bwhash` asserts only
       that the shift/xor chain COMPLETED — before the family had concolic semantics it aborted the document,
       so the endpoint's existence is the measurement. `bwtramp` is the mixed shape: an unknown on the left and
       the page's own suspending valueOf on the right. */
    int bwfork_tt  = (strstr(js, "\"/api/bwfork\"") && strstr(js, "bwwide") && strstr(js, "bwnarrow"));
    int bwhash_tt  = (strstr(js, "\"/api/bwhash\"") != NULL);
    int bwtramp_tt = (strstr(js, "\"/api/bwtramp\"") && strstr(js, "bwand") && strstr(js, "bwzero"));
    /* the controller's `abort` listener actually ran, and the timeout's unknown flag forked both ways */
    int abortfire_tt = (strstr(js, "\"/api/aborted\"") && strstr(js, "fired"));
    /* a DOMString argument coerced through the PAGE's toString (with a loop in it), then used as the real
       event type — the listener firing proves the coerced value was the one it registered under */
    int idlcoerce_tt = (strstr(js, "\"/api/idlcoerce\"") && strstr(js, "coerced"));
    /* THE DOM INTERFACES. Each is one spec sentence the components used to get wrong, and each answers with a
       token that only the correct behaviour can produce — `percopy`/`wrong`/`child` are what the old code said. */
    int domproto_tt = (strstr(js, "\"/api/protoid\"") && strstr(js, "shared") && !strstr(js, "percopy"));
    int cdnull_tt   = (strstr(js, "\"/api/cdnull\"")  && strstr(js, "empty"));
    int tcnull_tt   = (strstr(js, "\"/api/tcnull\"")  && strstr(js, "nochild") && !strstr(js, "\"child\""));
    int nodeval_tt  = (strstr(js, "\"/api/nodeval\"") && strstr(js, "\"null\""));
    int tcset_tt    = (strstr(js, "\"/api/tcset\"")   && strstr(js, "tcSET"));
    /* §13.15.3 STEP 1.c OVER THE EXAMPLES, three separate claims. `addfork` is the INVARIANT: the sum stayed
       unknown, so both arms ran — a numeric arm that answered with a bare number would decide the branch and
       lose one endpoint. `addnum` is the MEASUREMENT: `num` cancels, so §6.1.6.1.7 Number::add ( x, y )
       leaves exactly `v1000000` in the path parameter endpoint.c aligns, where a string concatenation leaves
       `v19200998080` — the quoted match is exact, so the old answer cannot satisfy it, which is why the
       constant is one the two arms spell differently. `addstr` is the string arm, required to stay byte-for-
       byte what it was: a String on either side takes step 1.c whatever the other side's example is. */
    int addfork_tt  = (strstr(js, "\"/api/addfork\"") && strstr(js, "addpos") && strstr(js, "addneg"));
    int addnum_tt   = (strstr(js, "/api/addnum/")     && strstr(js, "\"v1000000\""));
    int addstr_tt   = (strstr(js, "/api/addstr/")     && strstr(js, "\"r1920\""));
    /* §4.12.1.1's "immediately execute the script element" AT A POSITION INSIDE THE DOCUMENT'S OWN SEQUENCE.
       The match is EXACT and the near-miss is the whole point: `ABXC` is the injected program at the slot after
       the <script> that injected it, and `ABC` is that same program pushed to the TAIL — the only place a
       scheduler whose cursor walked the document's scripts before the flow's own rows could put it, and the
       place from which it runs after the program that reports. `ABC` must NOT satisfy this row, which is why
       the token is quoted whole rather than searched for as a substring of the sequence. */
    int scriptorder_tt = (strstr(js, "\"/api/scriptorder\"") && strstr(js, "\"ABXC\""));

    /* HTML §3.1.7's `currentScript`, in THREE rows because these are three independent claims and one boolean
       over them would say only that one of three unrelated things is wrong.
       `current-script` is the GETTER: the running element, answered per element, with §4.12.1.1's classic arm
       having set it — `csA:inline|csC` is script A's own id and the absence of a `src` on it, reported by
       script C under C's own id. A slot that leaked between programs reads `csA:inline|csA`; a slot that leaked
       between FLOWS reads `csINJ` for the arm that did not inject.
       `current-script-restore` is STEP 4 observed from outside the bracket — the timer task and the module arm
       both have to see null, and they are one row because they are one claim about the slot being empty when
       no script element is executing.
       `current-script-inject` is the arm that INJECTED: §4.12.1.1's "immediately execute the script element"
       runs `csINJ` at the slot after the program that appended it, and the element that program reads is its
       OWN — not the one that injected it, which is what a slot written by the injecting program would give. */
    int cscurrent_tt = (strstr(js, "\"/api/csend\"") && strstr(js, "\"csA:inline|csC\""));
    int csrestore_tt = (strstr(js, "\"/api/cstimer\"") && strstr(js, "\"/api/csmod\"") &&
                        !strstr(js, "\"LEAK\""));
    int csinject_tt = (strstr(js, "\"/api/csinj\"") && strstr(js, "\"csINJ\""));
    /* The §4.4 algorithms, each proved by its own endpoint carrying a token only the right answer produces. */
    static const char *const NODE_ALGOS[][2] = {
        { "\"/api/nodeconst\"",   "isconst" },   /* Node.ELEMENT_NODE, the constants on the interface object */
        { "\"/api/docnode\"",     "isnode"  },   /* Document IS a Node: nodeType 9, and contains() walks */
        { "\"/api/baseuri\"",     "base"    },
        { "\"/api/rootnode\"",    "isroot"  },   /* the option read parked on a page getter and resumed */
        { "\"/api/equalnode\"",   "iseq"    },   /* cloneNode(true) then isEqualNode/isSameNode */
        { "\"/api/deepequal\"",  "isdeep"  },   /* …and a nested chain of it, parking at every pair */
        { "\"/api/normalize\"",   "AB"      },   /* two Text children merged into one */
        { "\"/api/insertbefore\"","isfirst" },
        { "\"/api/connected\"",   "isconn"  },
        { "\"/api/position\"",    "isdisc"  },
        /* the four arms of compareDocumentPosition the disconnected assertion never reached */
        { "\"/api/posbits\"",    "containedby" },
        /* normalize's inner run: 3 children left (the merged text, the <hr>, the merged tail), and the two
           merged strings are the 40 digits and the 5 z's — an off-by-one in the run gives a shorter one */
        { "\"/api/normrun\"",    "0123456789012345678901234567890123456789" },
        { "\"/api/iface\"",       "isiface" },   /* instanceof up the chain + inherited interface constants */
        { "\"/api/event\"",       "isevent" },   /* new Event(type, EventInit) with a dictionary getter */
        { "\"/api/evcancel\"",    "iscancel"},   /* preventDefault over the canceled slot */
        { "\"/api/dispatch\"",    "isdispatch"},/* §2.9 synchronous dispatch, suspended inside a listener */
        { "\"/api/stopimmediate\"", "isstop" },
        { "\"/api/handler\"",     "ishandler"},  /* §8.1.7 the handler slot keeps its position */
        { "\"/api/handlernull\"", "isnull"  },
        { "\"/api/onglobal\"",    "isglobal"},   /* the mixins land on window and Document too */
        { "\"/api/iface2\"",     "isiface2"},   /* HTML's element-interface table, up the whole chain */
        /* §4.6.3: `a.href` is NOT the attribute — it RESOLVES against the document base and re-serialises, so
           `fetch(ia.href)` after `ia.href = '/api/reflected?v=isreflect'` requests the ABSOLUTE URL. This
           expected the relative one, which is what an attribute MIRROR answers and what a browser does not;
           the mirror is gone and the recorded endpoint is the resolved address. */
        { "\"https://x.test/api/reflected\"",  "isreflect"},
        { "\"/api/reflect2\"",   "isreflect2"},
        { "\"/api/reflectbool\"","isbool"  },   /* presence-based booleans, and removeAttribute */
        /* §2.6.1's URL reflection, which the audit cannot measure: absolute on read, raw in the attribute,
           path-relative resolved against the document's path, and an unparseable value read back verbatim. */
        { "\"/api/refurlabsent\"", "isempty" },
        { "\"/api/refurl\"",       "isabs"   },
        { "\"/api/refurlrel\"",    "isrel"   },
        { "\"/api/refurlbad\"",    "israw"   },
        { "\"/api/refurlproto\"",  "isone"   },
        /* §2.6.1's unsigned long: the range and the default are separate steps, overflow clamps rather than
           falling to the default, and the SETTER does neither. */
        { "\"/api/ulabsent\"",   "isdflt"   },
        { "\"/api/ulrange\"",    "isrange"  },
        { "\"/api/uldflt\"",     "isdflt2"  },
        { "\"/api/uloverflow\"", "isclamp"  },
        { "\"/api/ulset\"",      "isnoclamp"},
        { "\"/api/ulwindow\"",   "isdefaultwritten" },
        { "\"/api/ulmin\"",      "ismin"    },
        { "\"/api/click\"",      "isclick" },   /* §3.2.2 click() through the one dispatch machine */
        { "\"/api/cascade\"",    "isspec"  },   /* specificity beats document order — the cascade, not a list */
        { "\"/api/cssinline\"",  "isinline"},   /* inline layers over the author cascade */
        { "\"/api/cssua\"",      "isua"    },   /* the UA default, and the initial value below it */
        /* css-fonts-4 §2.5's computed font-size, and css-values-4 §6.1.1's two font-size-relative units. The
           `larger`, the `150%` and the `1.5em` INSIDE font-size are all a ratio of the PARENT's size, while
           the `2em` in a margin is a ratio of the element's OWN — §6.1.1's font-affecting rule is the whole of
           the difference. /api/fontroot has no token: its base is CSS_ENV_DEFAULT_FONT_SIZE, so what it proves
           is that a `rem` on the root resolved against the INITIAL size instead of recursing into itself. */
        { "\"/api/fontsize\"",   "isfs"    },
        { "\"/api/fontrem\"",    "isrem"   },
        { "\"/api/cssset\"",     "isset"   },   /* writes land in the style attribute, [SameObject] holds */
        { "\"/api/cssdel\"",     "isdel"   },
        { "\"/api/cssro\"",      "isro"    },   /* a computed declaration throws rather than silently ignoring */
        { "\"/api/bubble\"",     "isbubble"},   /* §2.9's path: target fixed, currentTarget and phase moving */
        { "\"/api/nobubble\"",   "isnobub" },   /* a non-bubbling event, and stopPropagation */
        { "\"/api/capture\"",    "iscap"   },   /* the CAPTURING leg, before the target and before bubbling */
        { "\"/api/dedup\"",      "isdedup" },   /* §2.7's key is (type, callback, capture) */
        { "\"/api/once\"",       "isonce1" },   /* removed BEFORE the call, so a re-entrant fire cannot re-run it */
        { "\"/api/dclbubble\"",  "isdcl"   },   /* the ENGINE's own fire walks the same path to the window */
        { "\"/api/abortsync\"",  "issync"  },   /* §3.2 abort() has run its listeners before it returns */
        { "\"/api/abortonce\"",  "isonce"  },   /* and a second abort() fires nothing */
        { "\"/api/formstate\"",  "isform"  },   /* a control's value state, the form's listed elements */
        { "\"/api/celife\"",     "isce"    },   /* §4.13 connectedCallback ran — code nothing else calls */
        { "\"/api/ceearly\"",    "isearly" },   /* the retroactive upgrade, and the wrapper's identity surviving */
        { "\"/api/ceasync\"",    "ceAWAIT" },   /* the reaction is a flow: it parked at a loop and at an await */
        { "\"/api/cename\"",     "issyntax"},   /* §4.13.1 a name with no hyphen is refused */
        { "\"/api/ceget\"",      "isget"   },
        { "\"/api/ceext\"",      "isext"   },   /* a DOMString dictionary member, read AND coerced as requests */
        { "\"/api/cedeep\"",     "isdeep"  },   /* the insertion steps walk the SUBTREE, and insertBefore runs them */
        { "\"/api/cegone\"",     "isgone"  },   /* …and the removing steps run disconnectedCallback */
        { "\"/api/ceattr\"",     "data-w"  },   /* attributeChangedCallback, for the OBSERVED name only */
        { "\"/api/classlist\"",  "iscl"    },   /* §7.1 add/remove/toggle/replace over the class attribute */
        { "\"/api/clview\"",     "isview"  },   /* …and the list is a VIEW, with nothing to keep in step */
        { "\"/api/clindex\"",    "isindex" },   /* §3.9 list[i], and §3.7.10's @@iterator */
        { "\"/api/matches\"",    "ismatch" },   /* §4.9 matches, and closest walking INCLUSIVE ancestors */
        { "\"/api/matchbad\"",   "issyn"   },
        { "\"/api/serialize\"",  "%3Cp%20class%3D%22q%22%3Ehi%3Cbr%3E%3C%2Fp%3E" },   /* §8.4, and `<br>` is void */
        { "\"/api/mixin\"",      "%3Cli%3E%3C%2Fli%3E%3Chr%3E%3Cli%3E%3C%2Fli%3E%3Cem%3E%3C%2Fem%3Etail" },
        { "\"/api/mixin2\"",     "%3Chr%3E%3Cli%3E%3C%2Fli%3E%3Cb%3E%3C%2Fb%3Etail" },
        { "\"/api/mixin3\"",     "%3Cspan%3E%3C%2Fspan%3E" },
        { "\"/api/variadic\"",   "%3Cli%3E%3C%2Fli%3En1770abcdefg" },   /* nine arguments, one a page toString */
        { "\"/api/live\"",       "islive"  },   /* childNodes and children track the tree; qSA does not */
        /* §4.2.6 moveBefore / §4.2.3 move — the three rows of the one block above. */
        { "\"/api/movebefore\"",      "ismoved" },       /* the reorder, and `moveBefore(n, n)` as a no-op */
        /* move's SIX validity steps where they diverge from pre-insert's eleven: step 1 first (which is why
           the fragment reads HierarchyRequestError and not step 4's), then step 3, then step 4's doctype. */
        { "\"/api/movebeforethrow\"",
          "HierarchyRequestError%3AHierarchyRequestError%3ANotFoundError%3AHierarchyRequestError" },
        { "\"/api/movebeforece\"",    "ispreserved" },   /* connectedMoveCallback ONLY — not the c/d pair */
        { "\"/api/named\"",      "isnamed" },
        /* §4.2.6 installed from ONE place: Document gets the reads it never had, and the lookups scope to
           whichever node they were called on rather than to the global document */
        { "\"/api/parentmixin\"", "scoped" },
        /* §4.7's interface, its two mixins, and §4.12.3's [SameObject] content */
        { "\"/api/fragment\"",   "content" },
        /* the upgrade is done by the time appendChild returns, and one insert ran 121 elements' steps */
        { "\"/api/insertsteps\"", "121" },
        /* 17 <li>, 17 <b>, the tail element's class, and 448 bytes of markup = 448 suspensions in one parse */
        { "\"/api/bigparse\"",   "448" },
        /* one JS object per node, over 34 of them reached two different ways */
        { "\"/api/nodeident\"",  "same" },
        /* 3 <i>, 2 with class a, 1 with both, 4 elements — then one more of each after an insert */
        { "\"/api/byname\"",     "4:3:2:5" },
        /* Text nodes only: the comment and the <template>'s content are both absent from `aBc` */
        { "\"/api/textwalk\"",   "aBc" },
        /* §4.2.6 scoped matching, the SyntaxError all four members owe, and closest's inclusive walk */
        { "\"/api/scopesel\"",   "SyntaxError:SyntaxError" },
        /* §3.2.2's mapping both ways, its two refusals, [SameObject], and the deleter */
        { "\"/api/dataset\"",    "roleName,userId,x" },
        /* the map, its iterator, the value setter, the NotFoundError, and an Attr's node identity */
        { "\"/api/attrs\"",      "node" },
        /* 1 form -> 2 after an insert (live), the two hrefs, and a fragment that empties when appended */
        { "\"/api/docshort\"",   "/l1,/l2" },
        /* the index cache: forwards, backwards, and shifted by a front insertion between two reads */
        { "\"/api/idxcache\"",   "shifted" },
        { "\"/api/adjacent\"",   "%3Ci%3Ebb%3C%2Fi%3E%3Cp%3ET%3Cb%3Eab%3C%2Fb%3E%3Cu%3Ebe%3C%2Fu%3E%3Cq%3E%3C%2Fq%3E%3C%2Fp%3E%3Cs%3Eae%3C%2Fs%3E" },
        { "\"/api/fragctx\"",    "%3Ctd%3Ecell%3C%2Ftd%3E" },   /* §13.4: parsed in the ROW's context */
        { "\"/api/outerset\"",   "%3Ch1%3Ereplaced%3C%2Fh1%3E" },
        { "\"/api/adjbad\"",     "issyn"   },
        { "\"/api/heading\"",    "%3Ch1%3Ea%3C%2Fh1%3Etail" },   /* the heading CLOSES; `tail` is its sibling */
        /* §4.10 the walk LEAVES THE TREE for `<template>`'s content fragment and comes back, twice over. */
        { "\"/api/template\"",   "%3Ctemplate%3E%3Ci%3Ex%3C%2Fi%3E%3Ctemplate%3E%3Cb%3Ey%3C%2Fb%3E"
                                 "%3C%2Ftemplate%3Ez%3C%2Ftemplate%3Eafter" },
        { "\"/api/serdeep\"",    "64" },   /* 64 open, 64 close, the text once — 128 suspensions in one walk */
        { "\"/api/clone\"",      "untouched" },   /* §4.4: shallow/deep, attributes, detached, original intact */
        { "\"/api/clonetpl\"",   "%3Ctemplate%3E%3Cb%3Etc%3C%2Fb%3E%3C%2Ftemplate%3E" },
        /* a template's TWO child lists. The clone copies both — 1 ordinary child in, 1 out, and it is the <u>.
           The serialisation shows only the content, which is §13.3 replacing the template with its contents. */
        { "\"/api/tplboth\"",    "1:1" },
        { "\"/api/rejevent\"",  "isrej"   },   /* §8.1.7.5's cancelable event reached a page listener */
        { "\"/api/prereq\"",    "isreq"   },   /* a `required` dictionary member, enforced by the declaration */
        { "\"/api/prector\"",   "isctor"  },
    };
    /* EVERY ONE OF THESE NAMES ITSELF NOW (fold_row above). They were a hundred bare `nodealgo_tt = 0`
       assignments, and the row is currently 0, so what the gate has been reporting for this whole session is
       "one of a hundred spec statements is wrong" with no way to ask which — while two of the hundred are the
       only reason anyone looks at it. */
    int nodealgo_tt = 1;
    const char *nodealgo_why = NULL;
    /* THE WEAKEST-NAMED ONE, AND IT IS FIRST DELIBERATELY. `wrong` is the failure token of dozens of
       independent fixture statements, so this assertion genuinely cannot localise further — which is exactly
       why it must say so rather than share the silence of the ones that can. */
    fold_row(&nodealgo_tt, &nodealgo_why, !strstr(js, "wrong"),
             "some statement of the fixture recorded its `wrong` token");
    /* §4.10: submit() derived the GET; the named field carries its SOURCE rather than an invented value; a
       DISABLED control contributed nothing and an UNCHECKED box contributed nothing, both of which only an
       absence can prove; and the second submit AFTER checking it does include it. */
    fold_row(&nodealgo_tt, &nodealgo_why, strstr(js, "\"/api/search\"") && strstr(js, "{state}.q"),
             "§4.10 submit() derived the GET carrying the field's source");
    fold_row(&nodealgo_tt, &nodealgo_why, !strstr(js, "\"off\""),
             "§4.10 a DISABLED control contributed nothing");
    fold_row(&nodealgo_tt, &nodealgo_why, !!strstr(js, "\"agree\""),
             "§4.10 the box CHECKED before the second submit contributed");
    /* requestSubmit(): the CANCELLED form's action must never appear, and the uncancelled one's must. */
    fold_row(&nodealgo_tt, &nodealgo_why, !strstr(js, "/api/never"),
             "§4.10 requestSubmit()'s CANCELLED form never reached its action");
    fold_row(&nodealgo_tt, &nodealgo_why, strstr(js, "\"/api/didsubmit\"") && strstr(js, "\"rs\""),
             "§4.10 requestSubmit()'s uncancelled form reached its action");
    /* §4.13: the async reaction's loop ran to its END across the preempts, not to some suspended partial. */
    fold_row(&nodealgo_tt, &nodealgo_why, !!strstr(js, "\"44850\""),
             "§4.13 the async reaction's loop ran to its END across the preempts");
    /* The uncaught DOMException is NAMED in the report, not "an object with no own name/message". */
    fold_row(&nodealgo_tt, &nodealgo_why, !!strstr(js, "SyntaxError: not a valid custom element name"),
             "an uncaught DOMException is NAMED in the page-error report");
    /* §8.4 outerHTML is the same serialiser over the element ITSELF — its own tag and attributes included. */
    fold_row(&nodealgo_tt, &nodealgo_why,
             !!strstr(js, "%3Csection%20data-k%3D%22v%22%3E%3Cp%20class%3D%22q%22%3Ehi%3Cbr%3E%3C%2Fp%3E%3C%2Fsection%3E"),
             "§8.4 outerHTML serialised the element ITSELF, tag and attributes included");
    /* §4.13.3: the old value, the new one, the removal's null — and NOTHING for the unobserved attribute. */
    fold_row(&nodealgo_tt, &nodealgo_why, strstr(js, "\"first\"") && strstr(js, "\"second\""),
             "§4.13.3 attributeChangedCallback saw the old value and the new one");
    fold_row(&nodealgo_tt, &nodealgo_why, !strstr(js, "data-ignored") && !strstr(js, "\"nope\""),
             "§4.13.3 the UNOBSERVED attribute produced no reaction");
    /* §8.1.7.5: the rejection nobody handled is reported, and the one handled a microtask later is NOT. */
    fold_row(&nodealgo_tt, &nodealgo_why, !!strstr(js, "rejNOHANDLER"),
             "§8.1.7.5 the rejection nobody handled is reported");
    fold_row(&nodealgo_tt, &nodealgo_why, !strstr(js, "rejHANDLED"),
             "§8.1.7.5 the rejection handled a microtask later is NOT reported");
    /* …and the one a listener CANCELLED is not reported either — the page answered for it. */
    fold_row(&nodealgo_tt, &nodealgo_why, !strstr(js, "rejCANCEL"),
             "§8.1.7.5 the rejection a listener CANCELLED is not reported");
    for (unsigned ai = 0; ai < sizeof(NODE_ALGOS) / sizeof(NODE_ALGOS[0]); ai++)
        fold_row(&nodealgo_tt, &nodealgo_why,
                 strstr(js, NODE_ALGOS[ai][0]) && strstr(js, NODE_ALGOS[ai][1]), NODE_ALGOS[ai][0]);
    /* THE OUTCOME FORK, both halves. Positive: ONE run reached BOTH completions of `JSON.parse` over unknown
       text, which can only happen by a snapshot fork taken inside the builtin — the throw arm carries the real
       SyntaxError, so `catch` ran with a real Error object rather than a shape. Negative: the same builtin over
       a source whose delivery contradicts the parse forked NOTHING and threw, so a completion no value of that
       source can produce was never fabricated. */
    int jsonfork_tt = strstr(js, "\"/api/jsonok\"") && strstr(js, "\"/api/jsonthrew\"")
                   && strstr(js, "\"validValues\":[\"SyntaxError\"]")
                   && strstr(js, "\"/api/jsonrawthrew\"") && !strstr(js, "/api/jsonrawok");
    int domidl_tt   = domproto_tt && cdnull_tt && tcnull_tt && nodeval_tt && tcset_tt;
    int deadline_tt = (strstr(js, "\"/api/deadline\"") && strstr(js, "expired") && strstr(js, "live"));

    /* @S: the eval sink reached by concolic state.code, breakout constructed + fire-verified. Read from the
       ONE document above — there is no second line to keep in step with it. */
    const char *ss = js;
    /* @S JS: the eval sink's argument is scanned per ECMAScript §12 and the hole is in §12.9.4's
       SingleStringCharacters, so the DERIVED escape is that production's own exit and nothing longer — the
       quote, a `;` because §12.10 inserts none on the same line, and `//` to discard the page's orphaned
       closing quote. The deleted CANDS_JS wrote a second `;` that no §12 rule asks for, and the assertion
       carried it; a payload one byte longer than the state requires is exactly what a derivation must stop
       producing, so the expected text is the derivation's. @S HTML: fire-verified via Lexbor re-parse. */
    int s_eval = s_poc(ss, "eval", "{state}.code", "';X9()//");
    /* THE SECOND §12 STATE, and it is the half that distinguishes a derivation from a renamed table: the same
       sink class, a different lexical state, and an escape no fixed list contained. The expected text is
       JSON-escaped because the escape IS a LineTerminator — §12.4 puts it outside the comment — so what the
       report carries is a backslash and an `n`, not a byte a payload table could have spelled. */
    int s_evalc = s_poc(ss, "eval", "{state}.note", "\\nX9()");
    int s_html = s_poc(ss, "innerHTML", "{state}.html", "<svg onload=X9()>");
    int s_url = s_poc(ss, "location", "{state}.next", "javascript:X9()");
    /* THE SOURCE'S BROWSER TRANSFORM, asserted end to end: a breakout through the REAL Location, whose value
       the browser percent-encodes per the fragment set and prefixes with `#`. The apostrophe is not in that set,
       so `';X9()//` arrives intact and fires — and it fires through `'#';X9()//'`, the leading `#` included.
       Before the transform existed the payload was handed over raw, so this fired for the wrong reason and an
       HTML-context breakout through the same source would have fired too, which a browser would not. */
    int s_loc = s_poc(ss, "eval", LOCATION_HASH_SRC, "';X9()//");
    /* THE NEGATIVE HALF, and it is an assertion about the REPORT, not about a missing line. The same source
       into an HTML sink must produce NO PoC — the fragment set encodes `<`, so every HTML candidate arrives as
       `%3C` and parses as text — and must still be REPORTED, as a parked search carrying the encode set that
       defeated it. Asserting only "no PoC" would also pass if the sink were never detected, which is the false
       negative this half exists to catch; asserting the parked entry says the sink WAS reached and searched. */
    int s_park = strstr(ss, "\"sink\":\"innerHTML\",\"source\":\"" LOCATION_HASH_SRC "\",\"search\":\"parked\"")
              && s_field(ss, "innerHTML", LOCATION_HASH_SRC, "\"sourceEncodes\":\" \\\"<>`\"")
              && !strstr(ss, "\"source\":\"" LOCATION_HASH_SRC "\",\"poc\":\"<");
    /* AND THE MEASURED HALF OF THAT CONSTRAINT, which is what makes the negative a MEASUREMENT rather than an
       absence. `sourceEncodes` is the DECLARATION — what location.c states the browser percent-encodes — and a
       page that ran `decodeURIComponent` over its own fragment would receive every one of those bytes anyway,
       so the declaration alone cannot say why nothing fired. `sourceDelivers` is the subset a delivery probe
       of THIS search observed arriving at the sink, and empty is the strongest thing a parked markup search
       can say: not one of the five reaches the write, so §13.2.5.1's only exit is unreachable through this
       source and no re-derivation gets past it. Read inside the record, because the pair is a fact about THIS
       search and both fields are emitted for every entry that has a declaration. */
    int s_nodeliver = s_field(ss, "innerHTML", LOCATION_HASH_SRC, "\"sourceDelivers\":\"\"");
    /* THE POSITIVE HALF OF THE SAME MECHANISM. The attribute-value write of the same root is a REAL raw-hash
       XSS, and the escape is built entirely out of bytes the fragment set does not hold: the apostrophe leaves
       §13.2.5.37, the solidus is §13.2.5.39's second exit into §13.2.5.40 "Self-closing start tag state" whose
       "Anything else" reconsumes in §13.2.5.32 "Before attribute name state", the injected `src` is the
       resource an `onerror` needs to FAIL (the img carries none, so §13.2.5.33's duplicate-attribute rule does
       not discard ours), and the filler attribute absorbs the template's own closing quote. Every value is
       quoted because the solidus is not an exit from §13.2.5.38 "Attribute value (unquoted) state" — it is
       appended there — so an unquoted value would swallow the rest of the escape. A payload table could not
       have held this: it is a function of the source's measured byte set AND the element the hole is in. */
    int s_attr  = s_poc(ss, "innerHTML", "{" LOCATION_HASH_SRC "}.slice()",
                        "'/src='x'/onerror='X9()'/x9pad='");
    int s_attrd = s_field(ss, "innerHTML", "{" LOCATION_HASH_SRC "}.slice()", "\"sourceDelivers\":\"\"");
    /* THE STAGES BEHIND EACH OF THOSE ROWS. The rows above assert the PAYLOAD TEXT, which only a real
       derivation followed by a real fire can produce, and nothing here relaxes them — these say WHERE a run
       that has not got there yet has got to. `-derived` is `reached >= 1` on a search whose candidate count
       has grown past the one run its class opens with, which for a DERIVED class means the derivation
       constructed a breakout; SINK_URL declares SINK_DERIVE_NONE and opens with its two written-down vectors,
       so it has no `-derived` row and asking for one would be a claim about a mechanism that class has not
       got. */
    int st_eval  = s_stage(ss, "eval",      "{state}.code");
    int st_evalc = s_stage(ss, "eval",      "{state}.note");
    int st_html  = s_stage(ss, "innerHTML", "{state}.html");
    int st_url   = s_stage(ss, "location",  "{state}.next");
    int st_loc   = s_stage(ss, "eval",      LOCATION_HASH_SRC);
    int st_lpark = s_stage(ss, "innerHTML", LOCATION_HASH_SRC);
    int st_attr  = s_stage(ss, "innerHTML", "{" LOCATION_HASH_SRC "}.slice()");

    /* …AND THE TWO SETS ARE HELD AGAINST EACH OTHER, WHICH IS WHAT WOULD HAVE CAUGHT THE FALSE GREEN THE DAY
       IT APPEARED. A verdict row and its stage row are computed from the SAME bytes about the SAME record, so
       "this PoC is fire-verified" and "this record carries no PoC" cannot both be true — and they stood side
       by side in one smoke line (`s-url=1` beside `s-url-atsink=0`) for as long as the verdict was three loose
       greps over the whole document. Two rows that can disagree and are never compared are two rows the reader
       has to notice by eye, which is the same failure as a field nobody reads.
       ONE DIRECTION ONLY, because only one of them is an implication: a FIRED row demands S_FIRED, while a
       0 verdict beside S_FIRED would be a record whose PoC text is not the one the derivation must produce —
       which is a real state the fixture must be able to report rather than crash on, and it is exactly what a
       regressed derivation looks like. */
    DCHECK(!s_eval  || st_eval  == S_FIRED, "the @S eval verdict is green for a record whose own stage says it "
                                            "carries no PoC — one of the two is not reading the record it names");
    DCHECK(!s_evalc || st_evalc == S_FIRED, "the @S eval-comment verdict is green for a record whose own stage "
                                            "says it carries no PoC");
    DCHECK(!s_html  || st_html  == S_FIRED, "the @S markup verdict is green for a record whose own stage says "
                                            "it carries no PoC");
    DCHECK(!s_url   || st_url   == S_FIRED, "the @S URL verdict is green for a record whose own stage says it "
                                            "carries no PoC — this is the pair that stood contradictory in the "
                                            "smoke line, satisfied out of a parked search's own candidate list");
    DCHECK(!s_loc   || st_loc   == S_FIRED, "the @S fragment-source verdict is green for a record whose own "
                                            "stage says it carries no PoC");
    DCHECK(!s_attr  || st_attr  == S_FIRED, "the @S attribute-value verdict is green for a record whose own "
                                            "stage says it carries no PoC");
    /* AND THE TWO HALVES OF THE ONE SOURCE CANNOT BOTH BE THE SAME VERDICT, which is what makes this pair a
       measurement of the DERIVATION rather than of the transform. Both writes are fed from `location.hash`
       and both are markup sinks; the only thing that differs is the §13.2.5 state the bytes land in and
       therefore which spellings of an exit the source can carry. A build that fires BOTH has stopped applying
       the source's transform (the data state's only exit is `<`); a build that fires NEITHER has a derivation
       that stops at the first spelling, which is the defect this fixture was added for. */
    DCHECK(!(st_lpark == S_FIRED),
           "the markup sink fed the RAW fragment produced a fire-verified PoC — §13.2.5.1's data state is left "
           "only through `<`, which the fragment percent-encode set holds, so a PoC there means a candidate "
           "was delivered without the source's own transform being applied to it");

    /* THE PROBES, DECLARED ONCE. This was a 46-term conjunction and a separate printf listing 43 of them, which
       is two hand-maintained lists of the same thing — so a probe could be computed and joined to NEITHER, and
       one immediately was: `clone_body` asserted §6.4 clone() and the gate never read it, so the fixture
       reported PASS on a result nothing had checked. A row here is what makes a probe a probe: the gate walks
       it and the report walks it, and a probe that is not declared does not exist rather than silently passing.
       THE @S VERDICTS ARE ROWS TOO, for the same reason — they were a second conjunction with its own printf
       below this table. One table, one gate, one report, one selector.*/
    /* THE CROSS-SESSION ROUND TRIP'S OWN STATEMENTS, one per record kind at each end, because "it resumed" is
       the observation that cannot distinguish an exercised tier from an unexercised one. The park's four
       numbers and the resume's four are the SAME four, so a kind written at one end and not rebuilt at the
       other is visible as a pair rather than inferred from a total.
       A ROW THAT READS 0 IS THE FORCING FUNCTION, NOT A FLAKE. `park-deep` at 0 means the eviction landed
       before any flow had forked, so the residue is the `f-,<val>` a park before the first pick already writes
       and the segment rebuild did not run. `park-cand` at 0 means no @S candidate session was live when the
       park was taken, so no attacker text crossed and park_hex/park_unhex did not. Neither is a reason to
       soften the row — each names exactly which arm went unexercised, which is the whole reason the census is
       per kind, and fixture_want_park is where the moment is chosen. */
    int cold_park_wrote = g_sess == SESS_PARK && cold_park_records() > 0;
    int cold_park_deep  = g_sess == SESS_PARK && g_cp.segs > 0;
    int cold_park_cand  = g_sess == SESS_PARK && g_cp.cands > 0;
    int cold_resumed_any   = g_sess == SESS_RESUME && (g_cr.flows + g_cr.cands) > 0;
    int cold_resumed_segs  = g_sess == SESS_RESUME && g_cr.segs > 0;
    int cold_resumed_cand  = g_sess == SESS_RESUME && g_cr.cands > 0;
    /* AND THE ARM THAT CARRIES CODE THE PAGE NEVER RAN. `park-orphan` says the residue names a FUNCTION and not
       only paths; `resumed-orphan` says the rebuild turned that name back into a drive waiting for its body;
       `resumed-orphan-met` says a take in this session actually handed one over, which is the only one of the
       three that can distinguish a locator that works from a locator that merely round-trips as text.
       AND THE FOURTH IS THE ONE THAT CANNOT BE PASSED BY A LOCATOR THAT NAMES THE WRONG THING: no waiting drive
       FINISHED without a body. The document's uncalled function forks arms, so several flows carry ONE locator
       and every one of them has to be handed the body — a route that satisfied the first and left the rest
       would pass `resumed-orphan-met` and fail only here. It is the engine's own count, asked of it rather than
       parsed back out of a log line. */
    long orphan_met = 0, orphan_unmet = 0;
    int cold_park_orphan, cold_res_orphan, cold_res_orphan_met, cold_res_orphan_all;

    engine_orphan_claims(&orphan_met, &orphan_unmet);
    cold_park_orphan    = g_sess == SESS_PARK && g_cp.orphans > 0;
    cold_res_orphan     = g_sess == SESS_RESUME && g_cr.orphans > 0;
    cold_res_orphan_met = g_sess == SESS_RESUME && orphan_met > 0;
    cold_res_orphan_all = g_sess == SESS_RESUME && orphan_unmet == 0;
    /* AND THE QUESTION A PEER ASKED, HANDED BACK RATHER THAN CARRIED. `park-remoteop` is the row about the
       refusal that used to abort here: a park that MET a STARTED cross-agent operation — a program mid-run with
       the zone's rendezvous token on its row — and returned it. A 0 is not a flake and not a reason to soften
       the row: it says the park moment did not contain one, and the moment is chosen in fixture_cold_moment,
       which is where a fix for it belongs.
       `park-remoteop-once` is the LAST-HOLDER rule, and it is the only one of the two that a per-flow hand-back
       would fail. engine_perform attaches one question to EVERY live timeline, so a notice per holder would be
       one hand-back repeated once per member — and worse than noisy: the zone would be told to forget a token
       a surviving timeline is about to answer under, which its own assert catches one seam away from the engine
       that caused it. One question asked, many holders, exactly one notice, and `flows > 1` is what makes the
       `back == 1` half say something. */
    long retract_flows = 0, retract_started = 0, retract_back = 0;
    int cold_park_remoteop, cold_park_remoteop_once;

    engine_retract_census(&retract_flows, &retract_started, &retract_back);
    cold_park_remoteop      = g_sess == SESS_PARK && retract_started > 0;
    cold_park_remoteop_once = g_sess == SESS_PARK && retract_flows > 1 && retract_back == 1;
    /* AND THE REPLAY REACHED ITS SINK AGAIN — the strongest thing a resumed residue can be asked to say, and a
       correction of what this row used to ask. It read BOTH ARMS of the branch out of the @H surface, and that
       is a statement about a program this session does not run: the moment fixture_want_park picks is the moment
       the @S search is live, so the residue is mostly CANDIDATE SESSIONS. solve_flow_begin suppresses endpoint
       recording for exactly those, because a request built out of an injected breakout is an @S artifact and not
       an observed endpoint — so a residue of candidates emits no endpoints BY DESIGN, and the old row could
       only ever have passed because a candidate's fork used to drop its substitution and leave one arm behaving
       like an exploration flow (engine_sibling_assemble now carries it).
       WHAT THIS ASSERTS IS THE WHOLE ROUND TRIP AT ONCE, and nothing weaker satisfies it: the payload crossed as
       hex and came back through park_unhex, the sink class crossed as a NAME and was re-bound to solve.c's own
       table pointer by solve_resume_candidate, the flow replayed its recorded arms over the segments cold_resume
       rebuilt, re-reached the eval sink and X9 FIRED there — which §@S says is the only thing that proves a PoC.
       A session that rebuilt its flows and then went nowhere reports nothing here. */
    int cold_fired = (strstr(js, "\"sink\":\"eval\"") && strstr(js, "{state}.code")
                      && strstr(js, "';X9()//")) ? 1 : 0;
    /* EVERY ROW NAMES THE STATEMENT IT IS ABOUT, and the two cold sessions are two answers and not one: they run
       the same document and one is about what a park WROTE while the other is about what a resume REBUILT. */
    Probe probes[] = {
        { "uid-param", has_uid_param, "/api/u?uid=", SESS_EXPLORE },
        { "role-admin", role_admin, "/api/data?role=", SESS_EXPLORE },
        { "path-param", path_param, "/v1/users/", SESS_EXPLORE },
        { "body-param", body_param, "firstPost", SESS_EXPLORE },
        { "path-example", path_example, "/v1/vis/", SESS_EXPLORE },
        { "role-public", role_public, "/api/data?role=", SESS_EXPLORE },
        { "merged", merged, "/api/data?role=", SESS_EXPLORE },
        { "pinned", pinned, "/api/region/", SESS_EXPLORE },
        /* THE KEY IS THE STATEMENT THAT REACHES THE ENDPOINT, WHICH IS NOT ALWAYS THE ENDPOINT. This row reads
           `/api/admin/audit-log`, an address that is in a CHUNK the provider serves and in no document at all;
           what the document has is the `loadScript` behind the admin arm. A key is a substring of the PROGRAM,
           never of the answer. */
        { "lazy", lazy, "/chunk/admin.js", SESS_EXPLORE },
        { "dom-attr", dom_attr, "/api/whoami", SESS_EXPLORE },
        { "dom-node", dom_node, "/api/kid", SESS_EXPLORE },
        { "dom-tt", dom_tt, "/api/whoami", SESS_EXPLORE },
        { "accessor", accessor_tt, "/api/flag", SESS_EXPLORE },
        { "async", async_tt, "/api/then", SESS_EXPLORE },
        { "await", await_tt, "/api/await", SESS_EXPLORE },
        { "asynccall", asynccall_tt, "/api/asynccall", SESS_EXPLORE },
        { "throw", async_throw, "/api/caught", SESS_EXPLORE },
        { "preempt", async_preempt, "/api/asyncloop", SESS_EXPLORE },
        { "orphan", orphan_driven, "orphanNeverCalled", SESS_EXPLORE },
        { "orphan-gate", orphan_gate, "orphanNeverCalled", SESS_EXPLORE },
        { "orphan-loop", orphan_loop, "orphanLoops", SESS_EXPLORE },
        { "orphan-ident", orphan_ident, "orphanIdentity", SESS_EXPLORE },
        { "orphan-ccode", orphan_ccode, "orphanCharCode", SESS_EXPLORE },
        { "orphan-update", orphan_update, "orphanUpdate", SESS_EXPLORE },
        { "orphan-clamp", orphan_clamp, "orphanClamp", SESS_EXPLORE },
        { "fetch", fetch_await, "/api/config", SESS_EXPLORE },
        { "clone-body", clone_body, "/api/clonebody", SESS_EXPLORE },
        { "body-bytes", body_bytes, "/api/bodybytes", SESS_EXPLORE },
        { "body-iso", body_iso, "/api/bodyiso", SESS_EXPLORE },
        { "verb-key", verb_key, "/api/echo", SESS_EXPLORE },
        { "hdrs", hdrs, "/api/hdrs?", SESS_EXPLORE },
        { "hdr-proxy", hdrproxy, "/api/hdrproxy", SESS_EXPLORE },
        { "needs-auth", needsauth, "/api/needsauth", SESS_EXPLORE },
        { "hdr-iter", hdriter, "/api/hdriter", SESS_EXPLORE },
        { "hdr-seq", hdrseq, "/api/hdrseq", SESS_EXPLORE },
        { "hdr-record", hdrrec, "/api/hdrrec", SESS_EXPLORE },
        { "mp-escape", mpesc, "/api/mpesc", SESS_EXPLORE },
        { "pending", pending_await, "/api/lazy", SESS_EXPLORE },
        { "promise-state", promise_state, "/api/shared", SESS_EXPLORE },
        { "delete-iso", delete_iso, "/api/tok", SESS_EXPLORE },
        { "global-delete", global_delete, "/api/gdel", SESS_EXPLORE },
        { "floc-iso", floc_iso, "/api/floc", SESS_EXPLORE },
        { "ua", uafork_tt, "/api/uafork", SESS_EXPLORE },
        { "touch", touchfork_tt, "/api/touch", SESS_EXPLORE },
        { "layout", layoutfork_tt, "/api/layout", SESS_EXPLORE },
        { "listener-options", aelunk_tt, "/api/aelunk", SESS_EXPLORE },
        { "bitwise", bwfork_tt, "/api/bwfork", SESS_EXPLORE },
        { "bitwise-chain", bwhash_tt, "/api/bwhash", SESS_EXPLORE },
        { "bitwise-tramp", bwtramp_tt, "/api/bwtramp", SESS_EXPLORE },
        { "abort", abortfire_tt, "/api/aborted", SESS_EXPLORE },
        { "deadline", deadline_tt, "/api/deadline", SESS_EXPLORE },
        { "idl", idlcoerce_tt, "/api/idlcoerce", SESS_EXPLORE },
        { "dom-idl", domidl_tt, "/api/protoid", SESS_EXPLORE },
        { "node-algo", nodealgo_tt, "/api/nodeconst", SESS_EXPLORE, nodealgo_why },
        { "pushfork", pushfork_tt, "/api/pushfork", SESS_EXPLORE },
        { "mapfork", mapfork_tt, "/api/mapfork", SESS_EXPLORE },
        { "fefork", fefork_tt, "/api/fefork", SESS_EXPLORE },
        { "add-fork", addfork_tt, "/api/addfork", SESS_EXPLORE },
        { "add-number", addnum_tt, "/api/addnum", SESS_EXPLORE },
        { "add-string", addstr_tt, "/api/addstr", SESS_EXPLORE },
        { "script-order", scriptorder_tt, "/api/scriptorder", SESS_EXPLORE },
        { "current-script", cscurrent_tt, "/api/csend", SESS_EXPLORE },
        { "current-script-restore", csrestore_tt, "/api/cstimer", SESS_EXPLORE },
        { "current-script-inject", csinject_tt, "/api/csinj", SESS_EXPLORE },
        { "owfork", owfork_tt, "/api/owfork", SESS_EXPLORE },
        { "genfork", genfork_tt, "/api/genfork", SESS_EXPLORE },
        { "gen2fork", gen2fork_tt, "/api/gen2fork", SESS_EXPLORE },
        { "genofork", genofork_tt, "/api/genofork", SESS_EXPLORE },
        { "afromfork", afromfork_tt, "/api/afromfork", SESS_EXPLORE },
        { "spreadfork", spreadfork_tt, "/api/spreadfork", SESS_EXPLORE },
        { "setaddfork", setaddfork_tt, "/api/setaddfork", SESS_EXPLORE },
        { "setfork", setfork_tt, "/api/setfork", SESS_EXPLORE },
        { "mapmutfork", mapmutfork_tt, "/api/mapmutfork", SESS_EXPLORE },
        { "afsfork", afsfork_tt, "/api/afsfork", SESS_EXPLORE },
        { "paffork", paffork_tt, "/api/paffork?", SESS_EXPLORE },
        { "paf2fork", paf2fork_tt, "/api/paf2fork", SESS_EXPLORE },
        { "redfork", redfork_tt, "/api/redfork", SESS_EXPLORE },
        { "rerepfork", rerepfork_tt, "/api/rerepfork", SESS_EXPLORE },
        { "toprimfork", toprimfork_tt, "/api/toprimfork", SESS_EXPLORE },
        { "gcallfork", gcallfork_tt, "/api/gcallfork", SESS_EXPLORE },
        { "gapplyfork", gapplyfork_tt, "/api/gapplyfork", SESS_EXPLORE },
        { "grefapplyfork", grefapplyfork_tt, "/api/grefapplyfork", SESS_EXPLORE },
        { "hostreq", hostreq_tt, "/api/hostreq?", SESS_EXPLORE },
        { "hostreq-fork", hostreqfork_tt, "/api/hostreqfork", SESS_EXPLORE },
        { "json-fork", jsonfork_tt, "/api/jsonok", SESS_EXPLORE },
        { "nav-open", navopen_tt, "/api/navopen", SESS_EXPLORE },
        { "proxy-sop", sop_tt, "/api/sop?", SESS_EXPLORE },
        { "xdoc-read", xdocread_tt, "/api/xdocread", SESS_EXPLORE },
        { "xdoc-job", xdocjob_tt, "/api/xdocjob", SESS_EXPLORE },
        { "timer-order", timer_tt, "/api/timerfire", SESS_EXPLORE },
        { "timer-string-handler", timerstr_tt, "/api/timerstr", SESS_EXPLORE },
        { "timer-handle-counter", timerhandle_tt, "/api/timerhandle", SESS_EXPLORE },
        { "timer-unknown-delay-fork", unkdelay_tt, "/api/unkdelay", SESS_EXPLORE },
        { "iframe-nav", ifnav_tt, "/api/iframenav", SESS_EXPLORE },
        { "idb-open", idbopen_tt, "/api/idbopen", SESS_EXPLORE },
        { "idb-record", idbrec_tt, "/api/idbrec", SESS_EXPLORE },
        { "idb-record-taint", idbtaint_tt, "/api/idbrec", SESS_EXPLORE },
        { "loc-hash-param", lochash_tt, "/api/idbrec", SESS_EXPLORE },
        { "idb-index", idbidx_tt, "/api/idbidx", SESS_EXPLORE },
        { "idb-index-uniq", idbuniq_tt, "/api/idbuniq", SESS_EXPLORE },
        { "optiter", optiter_tt, "/api/optiter", SESS_EXPLORE },
        { "cssprop", cssprop_tt, "/api/cssprop?", SESS_EXPLORE },
        { "cssprop-text", csspropText_tt, "/api/csspropText", SESS_EXPLORE },
        { "css-nesting-selectortext", cssnestsel_tt, "/api/cssnestsel", SESS_EXPLORE },
        { "css-nesting-cascade", cssnest_tt, "/api/cssnest?", SESS_EXPLORE },
        { "css-nesting-group-rule", cssnestmedia_tt, "/api/cssnestmedia", SESS_EXPLORE },
        { "s-eval", s_eval, "state.code", SESS_EXPLORE },
        { "s-evalc", s_evalc, "state.note", SESS_EXPLORE },
        { "s-html", s_html, "state.html", SESS_EXPLORE },
        { "s-url", s_url, "state.next", SESS_EXPLORE },
        { "s-loc", s_loc, "location.hash", SESS_EXPLORE },
        { "s-park", s_park, "location.hash", SESS_EXPLORE },
        /* THE MEASURED CONSTRAINT BEHIND THE NEGATIVE, and the POSITIVE it is the counterpart of. */
        { "s-park-nodeliver", s_nodeliver, "location.hash", SESS_EXPLORE },
        { "s-attr", s_attr, "location.hash", SESS_EXPLORE },
        { "s-attr-nodeliver", s_attrd, "location.hash", SESS_EXPLORE },
        /* THE STAGES, so a 0 above names one. Each row carries its parent's key, so selection is unchanged. */
        { "s-eval-seen", st_eval >= S_SEEN, "state.code", SESS_EXPLORE },
        { "s-eval-ran", st_eval >= S_RAN, "state.code", SESS_EXPLORE },
        { "s-eval-atsink", st_eval >= S_ARRIVED, "state.code", SESS_EXPLORE },
        { "s-evalc-seen", st_evalc >= S_SEEN, "state.note", SESS_EXPLORE },
        { "s-evalc-ran", st_evalc >= S_RAN, "state.note", SESS_EXPLORE },
        { "s-evalc-atsink", st_evalc >= S_ARRIVED, "state.note", SESS_EXPLORE },
        { "s-html-seen", st_html >= S_SEEN, "state.html", SESS_EXPLORE },
        { "s-html-ran", st_html >= S_RAN, "state.html", SESS_EXPLORE },
        { "s-html-atsink", st_html >= S_ARRIVED, "state.html", SESS_EXPLORE },
        { "s-url-seen", st_url >= S_SEEN, "state.next", SESS_EXPLORE },
        { "s-url-ran", st_url >= S_RAN, "state.next", SESS_EXPLORE },
        { "s-url-atsink", st_url >= S_ARRIVED, "state.next", SESS_EXPLORE },
        { "s-loc-seen", st_loc >= S_SEEN, "location.hash", SESS_EXPLORE },
        { "s-loc-ran", st_loc >= S_RAN, "location.hash", SESS_EXPLORE },
        { "s-loc-atsink", st_loc >= S_ARRIVED, "location.hash", SESS_EXPLORE },
        /* AND THE NEGATIVE HALF'S MISSING PREMISE. `s-park` asserts the markup sink fed from `location.hash`
           produces NO PoC and is still REPORTED — but "the sink was reached and the search genuinely failed"
           was never actually asserted, because the only evidence the entry carried was `tried`, which is
           raised at SEED time. This is that premise: candidates of that search actually EXECUTED.
           IT IS `S_RAN` AND NOT `S_ARRIVED`, AND THE CHANGE IS THE MEASUREMENT ARRIVING RATHER THAN A ROW
           BEING WEAKENED. `reached` counts a BREAKOUT'S bytes arriving, and once the derivation is solved
           JOINTLY there is no breakout for this state to deliver: §13.2.5.1 is left only through `<`, the
           delivery probe OBSERVES that `<` does not reach the write, and the derivation therefore constructs
           nothing rather than spending a document re-run to arrive as `%3C`. Asserting an arrival would be
           asserting that the engine still builds the candidate it has just proved cannot work — and it would
           be schedule-dependent besides, since whether one is built at all now depends on which of the two
           probes the WFQ runs first. What replaces it as the statement of a genuine failure is
           `s-park-nodeliver`, which is the observation itself. */
        { "s-park-ran", st_lpark >= S_RAN, "location.hash", SESS_EXPLORE },
        /* AND THE POSITIVE SEARCH'S OWN STAGES, so a 0 on `s-attr` names one. */
        { "s-attr-seen", st_attr >= S_SEEN, "location.hash", SESS_EXPLORE },
        { "s-attr-ran", st_attr >= S_RAN, "location.hash", SESS_EXPLORE },
        { "s-attr-atsink", st_attr >= S_ARRIVED, "location.hash", SESS_EXPLORE },
        /* THE TWO COLD SESSIONS. Their key is the @S sink whose candidate sessions are what makes a park write
           a 'c' record at all, so the row still names a statement of the document it runs over; the SESSION is
           what tells the two apart, because they run the SAME document and one is about what a park WROTE while
           the other is about what a resume REBUILT out of it. */
        { "park-wrote", cold_park_wrote, "state.code", SESS_PARK },
        { "park-deep", cold_park_deep, "state.code", SESS_PARK },
        { "park-cand", cold_park_cand, "state.code", SESS_PARK },
        { "resumed", cold_resumed_any, "state.code", SESS_RESUME },
        { "resumed-segs", cold_resumed_segs, "state.code", SESS_RESUME },
        { "resumed-cand", cold_resumed_cand, "state.code", SESS_RESUME },
        { "resumed-fired", cold_fired, "state.code", SESS_RESUME },
        /* THE 'o' ARM AT BOTH ENDS. Keyed on the uncalled function itself, because a key is a substring of the
           PROGRAM and this row is a statement about that function and nothing else. */
        { "park-orphan", cold_park_orphan, "coldOrphan", SESS_PARK },
        { "resumed-orphan", cold_res_orphan, "coldOrphan", SESS_RESUME },
        { "resumed-orphan-met", cold_res_orphan_met, "coldOrphan", SESS_RESUME },
        { "resumed-orphan-all", cold_res_orphan_all, "coldOrphan", SESS_RESUME },
        /* KEYED ON THE FORK, because that is the program text these two statements are about. A peer's question
           is asked by the host and no document contains it — but what makes the hand-back a mechanism rather
           than a single free() is that ONE question is held by MANY timelines, and this is the line that makes
           this document have many. */
        { "park-remoteop", cold_park_remoteop, "cfg.admin", SESS_PARK },
        { "park-remoteop-once", cold_park_remoteop_once, "cfg.admin", SESS_PARK },
    };
    /* WHICH ROWS THIS INVOCATION CARRIES — its SESSION, and whether its document contains the statement. */
    int n = 0;
    for (unsigned pi = 0; pi < sizeof(probes) / sizeof(probes[0]); pi++) {
        /* A KEY NO DOCUMENT CONTAINS IS A PROBE ABOUT A PROGRAM THAT DOES NOT EXIST, and it CRASHES rather than
           being quietly dropped from every run — which is the one failure mode a derived selector introduces and
           the same defect as a corpus file the collector does not collect: the total looks complete. The message
           NAMES the row, so the whole block is dev-only: a release build would otherwise still format it. */
#if APICLIENT_DEV
        {
            char why[256];

            snprintf(why, sizeof why,
                     "the probe `%s` names a statement no document in this fixture makes — its key `%s` is in "
                     "none of the three, so the row would be selected by no run and assert nothing, while the "
                     "table it sits in reads as complete", probes[pi].name, probes[pi].key);
            DCHECK(strstr(HTML, probes[pi].key) || strstr(HTML_MIN, probes[pi].key) ||
                   strstr(HTML_COLD, probes[pi].key), why);
        }
#endif
        if (probes[pi].sess != g_sess) continue;
        if (!strstr(g_doc, probes[pi].key)) continue;
        DCHECK(n < cap, "more probes were selected than the caller has room for — the report would state a "
                        "verdict over a prefix of the table and call it the table");
        out[n++] = probes[pi];
    }
    /* AND THIS RUN ASSERTS SOMETHING. A selection of nothing reports `=> OK` over an empty conjunction, which is
       a PASS nothing checked — and with the membership derived, that is exactly what a document whose statements
       no row names would produce. */
    DCHECK(n > 0, "this invocation selected no probes at all — every row was excluded by its session or by its "
                  "key, so the run would report a verdict it never computed");
    return n;
}

/* THE TABLE, PRINTED — ONE writer, for the reason the table is one list: a second printf listing some of the
   rows is how a probe came to be computed and joined to neither. It is a STREAM and not a summary, because the
   moment below is reached (if it is reached) only when every row is 1: a run that never reaches it must still
   say WHICH row is 0, at the cadence the rest of the census reports at, or a fifteen-minute run that is killed
   by its harness produces nothing at all — which is what every native run of this fixture had been doing.

   A SAMPLE MAY NOT PRINT A VERDICT, AND WHILE IT DID, 177 OF 179 LINES OF A PASSING RUN SAID `FAIL`. That is
   measured, on `native min`: every sample taken before the last row flipped to 1 ended `=> FAIL`, the run
   finished `=> OK` with exit 0, and any reader — a person scrolling, a `tail`, a `grep ... | tail -1` against a
   still-open log — had a 99% chance of taking the opposite of the answer. The stream is right and stays: a
   killed run must still name WHICH row is 0. What was wrong is the TOKEN, because a sample cannot know that a
   0 row is a failure rather than a row that has not been reached yet, and printing `FAIL` claims it does.
   So the CALLER says which moment this is, because the caller is the only one that knows: the park hook is
   asking "is every row 1 yet", where all-1 IS the terminal moment and a 0 is INCOMPLETE; the report at exit is
   the run's verdict, where a 0 is genuinely FAIL. This composes with the harness backstop directly — a killed
   run's last line now reads INCOMPLETE beside a HUNG stage, which together say what happened, where the old
   pair said HUNG beside a FAIL that had nothing to do with the kill. */
static int probes_report(const char *js, bool final) {
    Probe rows[PROBE_MAX];
    int n = probes_eval(js, rows, PROBE_MAX), ok = 1, i;

    /* THE STATEMENT BEHIND A FOLDED 0, ON ITS OWN LINE AND BEFORE THE VERDICT. Before, because this function's
       own rule is that the LAST line of a killed run is the verdict — the file already carries the measurement
       that 177 of 179 samples of a passing run were read backwards by a `tail`, and appending sentences after
       it would put that back. A row with no `why` prints nothing: it already names itself. */
    for (i = 0; i < n; i++)
        if (!rows[i].ok && rows[i].why) printf("@H   %s: %s\n", rows[i].name, rows[i].why);
    printf("@H ");
    for (i = 0; i < n; i++) {
        ok = ok && rows[i].ok;
        printf("%s=%d ", rows[i].name, rows[i].ok);
    }
    printf("=> %s\n", ok ? "OK" : (final ? "FAIL" : "INCOMPLETE"));
    fflush(stdout);
    return ok;
}

/* THE EXPLORING SESSIONS' PARK MOMENT (solver/engine.h's park hook): this document has answered every statement
 * it makes, so this engine has what it came for and leaves memory with its residue WRITTEN — which for this host
 * is the `_park` array of the result document below, the same recipes the extension's host stores in IndexedDB.
 * It is a park and not a "stop" for the reason engine.h states: the residue holds flows suspended mid-frame, and
 * dropping one is a dropped work item that flow_release is right to abort on.
 *
 * HOW OFTEN IT IS ASKED is a SAMPLING RATE of a MEASUREMENT and not a budget: it decides when the verdict is
 * recomputed, never how much the frontier may explore, and the verdict cannot change without something being
 * emitted. It is the engine's own progress cadence (ENGINE_PROGRESS_EVERY units of engine_work_done), so the @H
 * stream lands beside the @PROGRESS/@COLD/@HEAP lines it has to be read against — a report at some other rate
 * would be a second cadence to keep in step.
 *
 * WHY IT IS SAMPLED AT ALL: the answer is a function of the whole result document, which has to be rendered to
 * be asked. Rendering it at every step boundary would put a full endpoint-surface serialization between two
 * quanta of a run that takes tens of thousands of them. */
#define PROBE_SAMPLE_EVERY 1000
static int fixture_have_answers(void) {
    static long next;
    char *js;
    int ok;

    if (engine_work_done() < next) return 0;
    next = engine_work_done() + PROBE_SAMPLE_EVERY;
    /* THE RESULT DOCUMENT, RENDERED AT A STEP BOUNDARY WITH NO FLOW SWITCHED IN — which is the position this
       hook is asked at, and is why it may be rendered from the session's realm at all: the flow stamp and the
       COW capture route are both down between two slices, so nothing this builds is attributed to a flow. */
    js = result_json(g_probe_ctx);
    CHECK(js, "the fixture could not render the result document — its verdict AND its completion moment are both "
              "functions of that one string, so the run can neither report nor decide it is finished");
    /* A SAMPLE, so a 0 row is INCOMPLETE and not a failure — this hook's whole question is whether every row is
       1 YET, and the run continues when the answer is no. */
    ok = probes_report(js, false);
    /* …AND THE @S SEARCHES THEMSELVES, BECAUSE THIS IS THE ONLY PLACE THEY CAN BE OBSERVED AT ALL. The probe
       row says whether a sink FIRED and nothing about how far the search that is trying to got — that is what
       the parked entry's own numbers are for (solve.h: tried, turns, reached, survived/survivedOf, escaped,
       payloads), and every one of them was being composed here, 1382 times in one run, and freed unread.
       THE COMPLETION PRINT CANNOT SUBSTITUTE FOR IT: `@RESULT` runs after run_scheduler returns, and a run
       whose frontier does not drain is killed by build.mjs's backstop before it gets there — so on exactly the
       runs where the @S half is stuck, the document exists 1382 times and is printed zero times.
       A TIME SERIES AND NOT A SNAPSHOT, which is the fact the reader needs and a final print could not give
       even if it ran: `tried` going 1 -> 2 on a derived class IS the context probe arriving and its derivation
       returning a breakout, and `tried` sitting at 1 for the whole run is that probe never arriving. Those are
       the two states a `reached:0` row cannot distinguish, and they take opposite work.
       IT IS THE SHIPPED WRITER'S OWN BYTES (solve_json_array, which is what result.c embeds), never a
       re-derivation out of `js` — §Testing: measure what the shipped path writes, or the number is a property
       of the instrument. */
    {
        char *sj = solve_json_array(g_probe_ctx);
        CHECK(sj, "the fixture could not render the @S search array — the parked entries are the only statement "
                  "this run makes about a security search that has not solved, and a sample without them "
                  "reports the @S half as silent when it is merely unprinted");
        printf("@S %s\n", sj);
        free(sj);
    }
    free(js);
    return ok;
}

/* THE --cold-park SESSION'S PARK MOMENT (solver/engine.h's park hook, the same seam the exploring one above
 * answers about the document instead). It is a POSITION IN THE RUN and not a budget: the park writes every
 * member of the frontier, so what this decides is when the residue leaves memory and never how much of it
 * there is.
 *
 * IT ASKS ABOUT THE RESIDUE, WHICH IS THE THING IT IS DECIDING ABOUT, and the version that did not is the whole
 * reason the read half of the cold tier had still never executed in any process. It was
 *     solve_candidate_count() > 0 && engine_host_owes()
 * and the second conjunct is FALSE BY CONSTRUCTION at the only point this seam is ever consulted: run_scheduler
 * asks the hook at the TOP of its loop, and the bottom of the previous iteration paid the provider and then
 * asserted, in two DCHECKs of its own, that `engine_pending_fetches()` and `engine_host_requests()` are BOTH empty.
 * engine_host_owes() walks for exactly the entries those two joins list, so it answered 0 every time it was
 * asked, for every document, in every session. Nothing said so: an unsatisfiable predicate and one that is
 * merely not true yet produce the identical run, and the @COLDPARK census reports zeroes for both. That is the
 * defaulted-field defect one level up — a question whose answer is fixed, read as a measurement.
 *
 * SO THE FACT IS ASKED OF THE COLD TIER (solver/cold.h's ColdPreview), which is the component that owns what a
 * residue IS, and it is ONE fact because the arms of the recipe grammar meet in one member: a CANDIDATE SESSION
 * THAT STANDS ON A FROZEN DECISION SEGMENT. A candidate is the only record that carries attacker text, hence the
 * only path through park_hex/park_unhex, and the only one whose sink class crosses by NAME and has to be re-bound
 * to a live pointer; standing on a segment is what makes the park write 's' records and the ordinals that name
 * them. So this moment is AFTER a candidate has re-entered the document and taken the branch, and not the moment
 * they were merely SEEDED, when their records are `c-,…` naming no segment at all.
 * IT IS `deepcands` AND NOT `cands > 0 && deep > 0`, which is the same statement about two counts and is a
 * WEAKER one: an exploring arm that has forked satisfies `deep` on its own. That mattered the moment candidates
 * began being seeded during exploration rather than after it (engine.c's pick), and the conjunction would have
 * gone on reading as though it said this.
 *
 * WHAT THIS MOMENT CANNOT CONTAIN is a DISCOVERY PROBE ('d'), and that is structural: active discovery is the
 * trusted zone's, so nothing in this engine seeds one. */
/* AND A DRIVE OF UNCALLED CODE BESIDE IT, which is the second half of the moment and not a refinement of the
 * first. `deepcands` says the residue will carry attacker text over a recorded path — the 'c' arm and the hex
 * — and says nothing whatever about the 'o' arm: an orphan drive is seeded only when a flow has run out of
 * everything else, so a park taken at the first deep candidate is taken BEFORE any drive exists and the record
 * that names a function is never written. The moment is where this file's own census comment says the fix for a
 * zero row belongs, so it is stated here rather than by softening the row.
 * IT CANNOT RACE THE DRIVE'S EXECUTION, which is why the preview is the right thing to ask: `orphan` is set
 * when the flow is ASSEMBLED, so this is true from the instant a drive is seeded and does not depend on it
 * having reached anything. */
/* AND THE THIRD HALF OF THE MOMENT, WHICH NO DOCUMENT CAN PUT THERE. A cross-agent OPERATION arrives from
 * another instance, so the frontier only holds one if a peer asked — and a park taken with one outstanding is
 * the state cold_park_flow REFUSED (with an abort, which killed the renderer and discarded every finding) until
 * the hand-back was built. Nothing in this tree could reach that state: this fixture is one instance, and every
 * host that can provision a second is a corpus runner. So the fixture stands in for the peer, which is exactly
 * what world_registry_selftest already does one component down.
 * THE MOMENT IS LATCHED, AND THE LATCH IS THE POINT rather than a convenience. The residue condition is a
 * property of a LIVE frontier — a candidate set drains — so a park that waits for two independent facts to be
 * true at the same instant can wait forever, and the run then hangs rather than failing a row. Once the residue
 * has been seen to hold what the park is a statement about, that stays a fact about this run.
 * IT IS ASKED AT THE MOMENT AND PARKED TWO CONSULTATIONS LATER, and the gap is what makes the exercise real
 * rather than hoped: engine_perform attaches the question to EVERY live timeline, the scheduler turns it into a
 * program on whichever flow it picks next, and only then does a member hold a STARTED operation. Parking on the
 * ask alone would meet the QUEUED half — the one that was already handed back — and never the started one. */
static int g_cold_moment, g_op_asked;

static int fixture_cold_moment(void) {
    ColdPreview would;

    /* THE SESSION THAT PARKS IS THE ONLY ONE WITH A PEER, asked of `g_sess` rather than of a second flag: the
       provider runs in all three, and which session this is is already stated once. */
    if (!g_cold_moment && g_sess == SESS_PARK) {
        cold_park_preview(&would);
        g_cold_moment = would.deepcands > 0 && would.orphans > 0;
    }
    return g_cold_moment;
}

/* THE QUESTION, ASKED ONCE, THROUGH THE PRODUCTION DOOR. `windowproxy.get … length` is HTML §7.2.1.3.1
   "CrossOriginProperties ( O )"'s `{ [[Property]]: "length", [[NeedsGetter]]: true }` — a member a cross-origin
   WindowProxy really does expose — and §7.2.2.2 "Indexed access on the Window object" says what answering it
   costs ("The length getter steps are to return this's associated Document's document-tree child navigables's
   size"), which is why it is the cheapest real operation to ask: it reads, it allocates no navigable, and it
   cannot fork. */
static void fixture_ask_remote_op(JSContext *ctx) {
    WorldId peer = { world_doc_intern("opeer"), 1, 0 };
    const char *const *carried;
    const char *vector = NULL;
    char rec[512];
    int n, i;

    if (g_op_asked) return;
    g_op_asked = 1;
    /* THE PEER'S WORLD IN THE ONE WIRE FORM THERE IS. `world_serialize` refuses a world this instance did not
       mint — correctly: only the minting instance holds the fork edges — so a fixture standing in for a peer
       cannot call it, and composing the text here would be the second spelling of a grammar html_iframe.c was
       already caught writing twice. The segment table holds the vector its own writer produced, so the arrival
       is performed first (which is what a real one does) and the text is read back out of it. */
    world_segment(ctx, peer, NULL, 0);
    n = world_segments_park(&carried);
    for (i = 0; i < n; i++) {
        WorldId back;
        const WorldId *anc;

        world_parse(carried[i], &back, &anc);
        if (world_eq(back, peer)) { vector = carried[i]; break; }
    }
    CHECK(vector != NULL,
          "the peer world this fixture just materialized is not in the segment table it was materialized into "
          "— the operation below would name a timeline this instance does not hold, and the park's own "
          "hand-back would be exercised against a question no flow was ever given");
    snprintf(rec, sizeof rec, "windowproxy.get\t%s\t%s\tlength", world_doc_name(world_local_doc()), vector);
    /* THE TOKEN IS THE TRUSTED ZONE'S NAME and this fixture is standing in for the zone, so it mints one. It is
       the thing that may never enter a recipe (solver/engine.h), which is the whole reason the park hands the
       question back instead of carrying it. */
    engine_perform(ctx, "tf-remoteop", rec);
}

static int fixture_want_park(void) {
    return fixture_cold_moment() && engine_operations_started() > 0;
}

/* XML 1.0 (Fifth Edition) §2.2 Characters, §2.3 Common Syntactic Constructs' [3] `S`, and §2.11 End-of-Line
 * Handling — core/xml/xml_char.h, the layer every other XML production reads its input through.
 *
 * TWO HALVES AND NEITHER SUBSUMES THE OTHER. The class predicates are asked at the BOUNDARIES the standard's
 * own ranges create, because a hand-transcribed range table is wrong at a boundary if it is wrong anywhere —
 * one row off and a code point silently changes class with no compiler and no other test to say so. The reader
 * is then driven over whole entities whose every character, every position and every fatal error is stated,
 * because §2.11's normalization and the line counter it defines exist ONLY there: a predicate can be perfect
 * and a reader can still collapse two line breaks into one.
 *
 * IT IS PURE C AND NEEDS NO REALM. This layer holds no JSValue, allocates nothing and has no flow — the reader
 * is a value, which is the property the parse above it will need in order to peek, fork and park, so a copy
 * round trip is exercised here rather than asserted in prose. */
static void xml_char_selftest(void)
{
    static const struct { uint32_t cp; bool ch, s; const char *why; } CLASS[] = {
        { 0x0000, false, false,
          "§2.2 [2] Char's first alternative is #x9, so NUL is not a character an XML document may contain — "
          "which is why this layer carries a length and never a terminator" },
        { 0x0008, false, false, "and neither is BACKSPACE: the gap below #x9 is why [2] writes three singletons" },
        { 0x0009, true,  true,  "[2]'s first alternative, and [3] S's `#x9`" },
        { 0x000A, true,  true,  "[2]'s second, and [3]'s `#xA`" },
        { 0x000B, false, false, "VERTICAL TAB sits between [2]'s singletons and is in neither production" },
        { 0x000D, true,  true,
          "#xD is a Char, and §2.3's note keeps it in S for the character reference that is the only way a "
          "production can ever match one — §2.11 has removed every literal #xD before a grammar looks" },
        { 0x000E, false, false, "[2] resumes at #x20, so SHIFT OUT is not a Char" },
        { 0x001F, false, false, "nor the last control below that run" },
        { 0x0020, true,  true,  "[#x20-#xD7FF] opens at SPACE, which is also [3]'s first alternative" },
        { 0x0021, true,  false, "the character after it is a Char and is not white space" },
        { 0xD7FF, true,  false, "the last code point before the surrogate blocks" },
        { 0xD800, false, false, "[2]'s own gloss: `excluding the surrogate blocks` — the first of them" },
        { 0xDFFF, false, false, "and the last" },
        { 0xE000, true,  false, "[#xE000-#xFFFD] resumes immediately above them" },
        { 0xFFFD, true,  false, "REPLACEMENT CHARACTER closes that range and IS a Char like any other" },
        { 0xFFFE, false, false, "[2]'s gloss excludes FFFE by name" },
        { 0xFFFF, false, false, "and FFFF" },
        { 0x10000, true, false, "[#x10000-#x10FFFF] is the whole of the supplementary planes" },
        { 0x10FFFF, true, false, "up to Unicode's last code point" },
        { 0x110000, false, false, "and no further — there is no code point above it for [2] to admit" },
        { XML_CHAR_EOF, false, false,
          "the end of the entity is not a character, which is why the sentinel is above [2]'s ceiling rather "
          "than inside the range it would otherwise have to be excluded from" },
    };
    /* THE ENTITIES, and what the reader must produce from each. `line`/`column` is where the reader STANDS when
       it stops — after the last character for a well-formed entity, ON the offending one for a fatal error,
       which is the position a `parsererror` has to quote. */
    static const uint32_t R_CRLF[]   = { 'a', 0x0A, 'b' };
    static const uint32_t R_CRCRLF[] = { 'a', 0x0A, 0x0A, 'b' };
    static const uint32_t R_LFCR[]   = { 'a', 0x0A, 0x0A, 'b' };
    static const uint32_t R_ACUTE[]  = { 0x00E9, '!' };
    static const uint32_t R_ASTRAL[] = { 0x1F600 };
    static const uint32_t R_FFFD[]   = { 0xFFFD };
    static const uint32_t R_TABSP[]  = { 0x09, 0x20 };
    static const uint32_t R_A[]      = { 'a' };
    static const struct { const char *in; size_t n; const uint32_t *want; size_t want_n;
                          XmlCharError err; size_t line, column; const char *why; } READ[] = {
        { "a\r\nb", 4, R_CRLF, 3, XML_CHAR_OK, 2, 2,
          "§2.11: `the two-character sequence #xD #xA` is ONE #xA, so this entity is three characters on two "
          "lines and not four on three" },
        { "a\rb", 3, R_CRLF, 3, XML_CHAR_OK, 2, 2,
          "and `any #xD that is not followed by #xA` is the same single #xA — the two spellings of a line "
          "break must be indistinguishable above this layer" },
        { "a\r\r\nb", 5, R_CRCRLF, 4, XML_CHAR_OK, 3, 2,
          "a bare #xD followed by a #xD#xA is TWO breaks: the lookahead is exactly one character deep, so a "
          "reader that swallowed greedily would lose a line" },
        { "a\n\rb", 4, R_LFCR, 4, XML_CHAR_OK, 3, 2,
          "and #xA#xD is two breaks as well — the sequence §2.11 collapses is #xD#xA, in that order" },
        { "\xC3\xA9!", 3, R_ACUTE, 2, XML_CHAR_OK, 1, 3,
          "`column` counts CHARACTERS: U+00E9 is two bytes and one column, which is the whole reason this "
          "position is the reader's and not a byte offset" },
        { "\xF0\x9F\x98\x80", 4, R_ASTRAL, 1, XML_CHAR_OK, 1, 2,
          "and a supplementary character is four bytes and one column" },
        { "\xEF\xBF\xBD", 3, R_FFFD, 1, XML_CHAR_OK, 1, 2,
          "U+FFFD closes [#xE000-#xFFFD] and is an ordinary Char — a decoder that substituted it for errors "
          "would make this entity indistinguishable from an ill-formed one" },
        { "\t ", 2, R_TABSP, 2, XML_CHAR_OK, 1, 3,
          "[3] S's characters are ordinary characters to the reader: the `+` and where a run is required are "
          "each grammar rule's own business" },
        { "", 0, NULL, 0, XML_CHAR_OK, 1, 1,
          "an empty entity ends at the position it started — a document that ends immediately is a thing this "
          "reader answers about, not the absence of one" },
        { "a\x01" "b", 3, R_A, 1, XML_CHAR_ERR_NOT_A_CHAR, 1, 2,
          "§2.2: U+0001 is below [2]'s first alternative, and the reader stops ON it — column 2 is the "
          "character the report has to name" },
        { "a\0b", 3, R_A, 1, XML_CHAR_ERR_NOT_A_CHAR, 1, 2,
          "a NUL is the same fatal error and not an end of input, which is what a terminator would have made it" },
        { "\xEF\xBF\xBE", 3, NULL, 0, XML_CHAR_ERR_NOT_A_CHAR, 1, 1,
          "U+FFFE is well-formed UTF-8 and excluded by [2]'s own gloss — the two fatal errors are different "
          "sentences and a report that merged them would send an author to the wrong one" },
        { "a\xC3", 2, R_A, 1, XML_CHAR_ERR_ILL_FORMED_UTF8, 1, 2,
          "§4.3.3: a sequence the entity ends inside is ill-formed, not a request for more bytes — this reader "
          "is handed the whole entity, so there are none" },
        { "a\x80", 2, R_A, 1, XML_CHAR_ERR_ILL_FORMED_UTF8, 1, 2,
          "and a continuation byte with no lead byte is ill-formed" },
        { "\xC0\x80", 2, NULL, 0, XML_CHAR_ERR_ILL_FORMED_UTF8, 1, 1,
          "an overlong encoding of NUL is refused as an ENCODING error before [2] is ever asked — Unicode 3.9 "
          "makes it ill-formed, and §4.3.3 names that sentence" },
        { "\xED\xA0\x80", 3, NULL, 0, XML_CHAR_ERR_ILL_FORMED_UTF8, 1, 1,
          "the UTF-8 encoding of an unpaired surrogate is excluded twice over — by §4.3.3 as an encoding and "
          "by [2] as a character — and the encoding sentence is reached first, which is the answer a report "
          "quotes and therefore the one this engine must be definite about" },
    };
    size_t i;

    for (i = 0; i < sizeof(CLASS) / sizeof(CLASS[0]); i++) {
        CHECK(xml_char_is_char(CLASS[i].cp) == CLASS[i].ch, CLASS[i].why);
        CHECK(xml_char_is_s(CLASS[i].cp) == CLASS[i].s, CLASS[i].why);
        /* Every alternative of [3] S is one of [2]'s first four, so a white-space character that is not a Char
           is the two tables disagreeing about the same four code points. */
        CHECK(!xml_char_is_s(CLASS[i].cp) || xml_char_is_char(CLASS[i].cp),
              "a code point is [3] S and not [2] Char — S's four alternatives are all Chars, so the two "
              "transcriptions have drifted apart");
    }
    CHECK(strcmp(xml_char_error_message(XML_CHAR_ERR_ILL_FORMED_UTF8),
                 xml_char_error_message(XML_CHAR_ERR_NOT_A_CHAR)) != 0,
          "the two fatal errors this layer decides carry one message — §4.3.3's ill-formed byte sequence and "
          "§2.2's illegal character are different mistakes and an author has to be told which one it was");
    for (i = 0; i < sizeof(READ) / sizeof(READ[0]); i++) {
        XmlCharReader r, before;
        uint32_t cp = 0;
        XmlCharError e;
        size_t got = 0;

        xml_char_reader_init(&r, READ[i].in, READ[i].n);
        CHECK(r.p == r.start && r.line == 1 && r.column == 1 && r.fatal == XML_CHAR_OK,
              "a fresh XML character reader does not stand at its entity's first byte, line 1, column 1");
        for (;;) {
            before = r;
            e = xml_char_read(&r, &cp);
            if (e != XML_CHAR_OK || cp == XML_CHAR_EOF) break;
            CHECK(got < READ[i].want_n && cp == READ[i].want[got], READ[i].why);
            got++;
        }
        CHECK(e == READ[i].err && got == READ[i].want_n, READ[i].why);
        CHECK(r.line == READ[i].line && r.column == READ[i].column, READ[i].why);
        CHECK(r.fatal == e, "an XML character reader's §1.2 latch disagrees with the error it just returned");
        CHECK(e == XML_CHAR_OK
                  || (r.p == before.p && r.line == before.line && r.column == before.column),
              "an XML character reader advanced past the character it reported a fatal error for — the "
              "position a `parsererror` quotes would then name the character AFTER the mistake");
        if (e == XML_CHAR_OK) {
            /* EOF IS IDEMPOTENT, because a tokenizer state that looks at the next character twice must see one
               entity and not two. */
            XmlCharReader again = r;
            uint32_t eof = 0;

            CHECK(xml_char_read(&again, &eof) == XML_CHAR_OK && eof == XML_CHAR_EOF
                      && again.p == r.p && again.line == r.line && again.column == r.column,
                  "reading at the end of an XML entity did not answer EOF again without moving");
        }
    }
    /* THE COPY IS THE PEEK AND THE COPY IS THE PARK — the property the parse above this will fork and suspend
       on, so it is exercised rather than described. */
    {
        static const char SRC[] = "x\r\ny";
        XmlCharReader r, saved;
        uint32_t cp = 0;
        size_t k;

        xml_char_reader_init(&r, SRC, sizeof SRC - 1);
        CHECK(xml_char_read(&r, &cp) == XML_CHAR_OK && cp == 'x', "the first character of a known entity");
        saved = r;
        for (k = 0; k < 3; k++) CHECK(xml_char_read(&r, &cp) == XML_CHAR_OK, "a known entity read to its end");
        CHECK(cp == XML_CHAR_EOF && r.line == 2 && r.column == 2, "a known entity's end position");
        r = saved;
        CHECK(xml_char_read(&r, &cp) == XML_CHAR_OK && cp == 0x0A && r.line == 2 && r.column == 1,
              "an XML character reader restored from a copy did not resume at the character and position it "
              "was copied at — peeking and parking ARE that copy, so a reader that cannot be rewound this way "
              "can neither fork an arm nor suspend a parse");
    }
}

/* CSS Properties and Values API 1 §3's `@property` GRAMMARS — its `<custom-property-name>#` prelude, its two
 * descriptors that have a grammar of their own, and §5.4's consume-a-syntax-definition that §3.1's validity is
 * stated by reference to.
 *
 * IT IS PURE C AND IT READS NO CSSOM MEMBER, and that is a rule and not a convenience: §6.1's four attributes
 * are IDL getters, and a getter is page-observable code that must run on the trampolined chain as a flow — a C
 * activation has no flow base under it, so a loop in a getter body would drive to completion instead of
 * parking. `JS_GetPropertyInternal` refuses one outright. So the two halves of this interface are tested from
 * the two places that can see them: the GRAMMARS here, where the parsing risk actually is and where one fixture
 * exercises one contract; and the four ATTRIBUTES from the fixture document's own JavaScript, where every other
 * page read in this engine happens (see `/api/cssprop` in HTML above and the two probe rows over it). */
static void css_property_grammar_selftest(void)
{
    static const struct { const char *s; bool valid, universal; const char *why; } SYNTAX[] = {
        { "*", true, true, "§5.4.2 step 3: a lone `*` IS the universal syntax definition" },
        { " * ", true, true, "§5.4.2 step 1 strips ASCII whitespace before step 3 measures the length" },
        { "", false, false, "§5.4.2 step 2: a zero-length string is failure" },
        { "   ", false, false, "and so is one that is nothing but the whitespace step 1 strips" },
        { "<length>", true, false, "§5.1 names `<length>`, so §5.4.4 returns it" },
        { " <color># ", true, false,
          "§5.2's `#` follows the name immediately; the whitespace around the whole string is step 1's" },
        { "<length> | <percentage>", true, false, "§5.3's combinator, with §5.4.2 step 6's whitespace" },
        { "<length>|auto", true, false, "and with none — the whitespace is `as much as possible`, not required" },
        { "big | bigger | BIGGER", true, false,
          "§5.1's ident arm is codepoint-wise, so three spellings are three components and not one" },
        { "I\\ dent|none", true, false,
          "§5.4.3's `\\` arm: an escaped space is part of the ident sequence, which is what makes "
          "css/css-properties-values-api/at-property-cssom.html's `--escape-syntax` a valid rule" },
        { "<LENGTH>", false, false,
          "§5.4.4 asks whether what it BUILT is a supported syntax component name and §5.1 spells all fifteen "
          "in lower case — neither section folds case, and §5.1's own note says `<custom-ident>`s are compared "
          "codepoint-wise" },
        { "<bogus>", false, false, "a data type name outside §5.1's fifteen is §5.4.4's `otherwise return failure`" },
        { "<length", false, false, "§5.4.4 runs to EOF without a `>`, which its `anything else` arm refuses" },
        { "inherit", false, false,
          "§5.4.3: `if component's name does not parse as a <custom-ident>, return failure`, and CSS Values "
          "§4.2 excludes the CSS-wide keywords in all ASCII case permutations" },
        { "DEFAULT", false, false, "§4.2 excludes the reserved `default` the same way, case included" },
        { "<transform-list>+", false, false,
          "§5.2: `any syntax component name EXCEPT pre-multiplied data type names may be immediately followed "
          "by a multiplier`, so §5.4.3 returns before the `+` and §5.4.2 step 7 meets it" },
        { "<transform-list>", true, false, "the same name unmultiplied is a component like any other" },
        { "* | <length>", false, false,
          "§5.1's note: `*` may not be combined with anything else — it is not a syntax component, so step 5 "
          "refuses it" },
        { "<length> <percentage>", false, false, "juxtaposition is not §5.3's combinator" },
        { "<length> |", false, false, "a trailing combinator leaves step 5 with nothing to consume" },
    };
    static const struct { const char *s; const char *want; const char *why; } SYN_DESC[] = {
        { "\"<length>\"", "<length>", "§3.1's `Value: <string>` denotes the string's own VALUE, unquoted" },
        { "\" <color># \"", " <color># ",
          "§6.1's `exactly as specified` keeps the spaces INSIDE the string — §5.4.2's strip is for the "
          "validity question alone, which is why css/css-properties-values-api/at-property-cssom.html reads "
          "`--valid-whitespace`'s syntax back with them" },
        { "\"I\\\\ dent|none\"", "I\\ dent|none", "the tokenizer unescapes the string, so `\\\\` is one backslash" },
        { "<length>", NULL, "an unquoted value is not a `<string>` and the descriptor is ignored" },
        { "\"a\" \"b\"", NULL, "`Value: <string>` is ONE value, so a second token matches no grammar" },
        { "", NULL, "and a descriptor with no value at all matches none either" },
    };
    static const struct { const char *s; bool ok, want; const char *why; } INHERITS[] = {
        { "true", true, true, "§3.2's `Value: true | false`" },
        { "FALSE", true, false, "a CSS keyword is ASCII case-insensitive" },
        { "1", false, false, "and nothing else is one of the two" },
        { "true false", false, false, "one value, so a second token matches no grammar" },
    };
    static const struct { const char *s; bool ok; unsigned n; const char *why; } NAMES[] = {
        { "--a", true, 1, "CSS Variables §2: a `<custom-property-name>` is a `<dashed-ident>`" },
        { "--switch-position", true, 1, "the shape a shipping site actually declares" },
        { " --a , --b , --c ", true, 3, "§3's `#` multiplier, whose commas may carry whitespace" },
        { "--", false, 0, "§2 reserves `--` itself, so it is not a custom property name" },
        { "a", false, 0, "an ident with no dashes is not a `<dashed-ident>`" },
        { "-a", false, 0, "and neither is one with a single dash" },
        { "--a,", false, 0, "`#` has no zero-length arm, so a trailing comma matches nothing" },
        { "", false, 0, "and neither does an empty prelude — `<custom-property-name>` is REQUIRED" },
        { "--a --b", false, 0, "the multiplier is comma-separated; juxtaposition is not it" },
    };
    /* THE COMPONENTS THEMSELVES, which are what §3.3 consumes. The table above only asks whether a string is a
       syntax definition; this one asks what definition it is, because a parse that dropped a component or lost
       a multiplier answers both of those questions identically and only diverges at the value match. */
    static const struct { const char *s; size_t n; const char *first; CssSyntaxMultiplier mult;
                          const char *why; } SHAPE[] = {
        { "<length>", 1, "<length>", CSS_SYNTAX_MULT_NONE, "§5.4.2 step 5 runs once for a lone component" },
        { "<length> | <percentage> | auto", 3, "<length>", CSS_SYNTAX_MULT_NONE,
          "§5.3: `syntax strings may use U+007C VERTICAL LINE (|) to provide multiple syntax component names. "
          "Such syntax strings will result in a syntax definition with multiple syntax components`" },
        { "<color>#", 1, "<color>", CSS_SYNTAX_MULT_COMMA, "§5.2's `#` indicates a comma-separated list" },
        { "<length>+", 1, "<length>", CSS_SYNTAX_MULT_SPACE, "§5.2's `+` indicates a space-separated list" },
        { "I\\ dent|none", 2, "I dent", CSS_SYNTAX_MULT_NONE,
          "§5.4.3's ident arm appends the UNESCAPED code point, so the component's name is the six-code-point "
          "`I dent` and that is what §5.1's codepoint-wise comparison is against" },
        { "<transform-list>", 1, "<transform-list>", CSS_SYNTAX_MULT_NONE,
          "§5.4.3 returns a pre-multiplied component BEFORE the multiplier is looked for, so its own `+` lives "
          "in the NAME and never in this field" },
    };
    /* §3.3's cross-descriptor condition, which is a VALUE parse: `if specified, the value of the initial-value
       descriptor must successfully parse according to the rule's syntax descriptor`, and §4.1 makes the
       non-universal arm `according to syntax definition`. Every row is one type grammar or one multiplier. */
    static const struct { const char *s, *v; bool want; const char *why; } MATCH[] = {
        { "<percentage>", "0", false,
          "CSS Values §5.5: a percentage is `a number immediately followed by a percent sign %`, which is the "
          "`<percentage-token>` production — a bare `0` is a `<number-token>`. This is developer.mozilla.org's "
          "own `@property --switch-position{syntax:\"<percentage>\";inherits:false;initial-value:0}`, whose "
          "`initialValue` must therefore be null" },
        { "<percentage>", "0%", true, "and the same rule written with the sign parses" },
        { "<length>", "0", true,
          "CSS Values §6: `for zero lengths the unit identifier is optional (i.e. can be syntactically "
          "represented as the <number> 0)`" },
        { "<length>", "1", false, "which is the ZERO alone — a unitless non-zero is a `<number>`" },
        { "<length>", "0px", true, "at-property-cssom.html's `--valid-reverse` initial value" },
        { "<length>", "2em", true,
          "§6.1.1's font-relative units are `<length>`s whether or not this engine can absolutize one — a "
          "grammar that refused them would report an unbuilt component as an author's mistake" },
        { "<length>", "50%", false, "a `<percentage>` is a sibling production of §6's, never a `<length>`" },
        { "<length>", "10deg", false, "and so is §7.1's `<angle>`" },
        { "<length>", "1px 2px", false, "an unmultiplied component is ONE component value and nothing after it" },
        { "<length-percentage>", "50%", true,
          "§5.1: `any valid <length> or <percentage> value`" },
        { "<number>", "1.5", true, "CSS Values §5.3's `<number-token>`" },
        { "<integer>", "-3", true,
          "CSS Values §5.2: `the first digit of an integer may be immediately preceded by - or + to indicate "
          "the integer's sign`" },
        { "<integer>", "1.5", false,
          "§5.2's integer is `a subset of the <number-token> production` — CSS Syntax §4.3.13 sets type to "
          "`number` at its fraction step" },
        { "<integer>", "1e3", false, "and at its exponent step, which is the same sentence of §4.3.13" },
        { "<angle>", "0.25turn", true, "CSS Values §7.1 names `turn`: `there is 1 turn in a full circle`" },
        { "<angle>", "0", false,
          "§7.1's note: the legacy bare-0 spelling `is not true in general, however, and will not occur in "
          "future uses of the <angle> type`" },
        { "<time>", "200ms", true, "CSS Values §7.2's two identifiers" },
        { "<time>", "200MS", true, "a CSS unit identifier is ASCII case-insensitive" },
        { "<resolution>", "2x", true, "CSS Values §7.4 lists `x` beside `dppx` with the same definition" },
        { "<resolution>", "-2dppx", false,
          "§7.4: `the allowed range of <resolution> values always excludes negative values`, and §5.1: `if the "
          "value is outside the allowed range ... the declaration is invalid and must be ignored`" },
        { "<string>", "\"hi\"", true, "CSS Values §4.4's `<string-token>`" },
        { "<string>", "hi", false, "an unquoted ident is not one" },
        { "<custom-ident>", "foo", true, "CSS Values §4.2's production" },
        { "<custom-ident>", "inherit", false,
          "§4.2: `the CSS-wide keywords are not valid <custom-ident>s`" },
        { "<custom-ident>", "DEFAULT", false,
          "§4.2 reserves `default` too, `excluded in all ASCII case permutations`" },
        { "<color>", "red", true, "CSS Color 4's named colours" },
        { "<color>", "rgb(1, 2, 3)", true,
          "a functional notation is ONE component value, commas included — CSS Syntax §5.5.8 consumes a "
          "function whole, so this is not a two-item list that `<color>` unmultiplied would refuse" },
        { "<color>", "notacolour", false, "and an ident that names no colour is not one" },
        { "<color> | none", "none", true,
          "§5.3: `the syntax components are matched in the order specified`, so the second one is asked" },
        { "<color> | none", "nope", false, "and when neither matches, the definition does not" },
        { "red | <color>", "red", true,
          "§5.3's own note: `given the syntax string \"red | <color>\", matching the value red against it will "
          "parse as an identifier`" },
        { "big | bigger", "BIGGER", false,
          "§5.1's note: `specifying an ident like Red means that the precise value Red is accepted; red, RED, "
          "and any other casing variants are not matched by this`" },
        { " <color># ", "red, blue", true,
          "§5.2's `#`, over at-property-cssom.html's `--valid-whitespace`; §5.4.2 step 1 stripped the spaces "
          "around the syntax string before the component was read" },
        { "<color>#", "red", true, "a comma-separated list of one is a list" },
        { "<color>#", "red,", false, "a separator with nothing after it is not a shorter one" },
        { "<color>#", "red blue", false, "and juxtaposition is `+`'s separator, not `#`'s" },
        { "<length>+", "1px 2px 3px", true, "§5.2's `+`" },
        { "<length>+", "1px, 2px", false, "whose separator is whitespace and not the comma" },
        { "I\\ dent|none", "I\\ dent", true,
          "at-property-cssom.html's `--escape-syntax`: both sides are UNESCAPED before they are compared, so "
          "the value's ident token and the component's name are the same six code points" },
        { "I\\ dent|none", "I dent", false,
          "and the same six code points written WITHOUT the escape are two ident tokens with whitespace "
          "between them, which is two component values and matches no unmultiplied component" },
        { "<url>", "url(a.png)", true,
          "CSS Values §4.5: the unquoted spelling `is specially-parsed as a <url-token>`, which is `<url()>`'s "
          "second arm" },
        { "<url>", "url(\"a.png\")", true, "and `url( <string> <url-modifier>* )` is its first" },
        { "<url>", "src(\"a.png\")", true, "§4.5's `<url> = <url()> | <src()>`" },
        { "<url>", "\"a.png\"", false,
          "§4.5's bare-string spelling belongs to `SOME CSS contexts (such as @import)` and is not the `<url>` "
          "production itself" },
    };
    unsigned i;

    for (i = 0; i < sizeof(SYNTAX) / sizeof(SYNTAX[0]); i++) {
        /* Pre-seeded with the WRONG answer and a sentinel count, so a refused parse that still wrote shows. */
        CssSyntaxDefinition def = { NULL, 99, SYNTAX[i].valid ? !SYNTAX[i].universal : true };
        bool got = css_property_syntax_definition(SYNTAX[i].s, strlen(SYNTAX[i].s), &def);

        CHECK(got == SYNTAX[i].valid, SYNTAX[i].why);
        CHECK(!got || def.universal == SYNTAX[i].universal, SYNTAX[i].why);
        /* §5.4.1's two outcomes are DISJOINT: the universal definition "accepts any valid token stream" and
           carries no components, and every other definition is a non-empty list of them. A refused string is
           neither and must leave nothing allocated — the entry's own contract, asserted rather than trusted. */
        CHECK(!got || (def.universal ? def.n == 0 : def.n > 0), SYNTAX[i].why);
        CHECK(got || (def.v == NULL && def.n == 0),
              "a refused syntax string left a partly-built definition allocated");
        css_syntax_definition_free(&def);
    }
    for (i = 0; i < sizeof(SHAPE) / sizeof(SHAPE[0]); i++) {
        CssSyntaxDefinition def = { NULL, 0, true };
        bool got = css_property_syntax_definition(SHAPE[i].s, strlen(SHAPE[i].s), &def);

        CHECK(got && !def.universal && def.n == SHAPE[i].n, SHAPE[i].why);
        CHECK(strcmp(def.v[0].name, SHAPE[i].first) == 0, SHAPE[i].why);
        CHECK(def.v[0].multiplier == SHAPE[i].mult, SHAPE[i].why);
        css_syntax_definition_free(&def);
    }
    for (i = 0; i < sizeof(MATCH) / sizeof(MATCH[0]); i++) {
        CssSyntaxDefinition def = { NULL, 0, true };
        bool got = css_property_syntax_definition(MATCH[i].s, strlen(MATCH[i].s), &def);

        CHECK(got && !def.universal,
              "a match case names a syntax string that is not a non-universal syntax definition");
        CHECK(css_property_syntax_matches(&def, MATCH[i].v, strlen(MATCH[i].v)) == MATCH[i].want, MATCH[i].why);
        css_syntax_definition_free(&def);
    }
    for (i = 0; i < sizeof(SYN_DESC) / sizeof(SYN_DESC[0]); i++) {
        char *got = css_property_descriptor_syntax(SYN_DESC[i].s, strlen(SYN_DESC[i].s));

        CHECK(!SYN_DESC[i].want == !got, SYN_DESC[i].why);
        CHECK(!got || strcmp(got, SYN_DESC[i].want) == 0, SYN_DESC[i].why);
        free(got);
    }
    for (i = 0; i < sizeof(INHERITS) / sizeof(INHERITS[0]); i++) {
        bool flag = !INHERITS[i].want;   /* the WRONG answer, so a refused descriptor that still wrote shows */
        bool ok = css_property_descriptor_inherits(INHERITS[i].s, strlen(INHERITS[i].s), &flag);

        CHECK(ok == INHERITS[i].ok, INHERITS[i].why);
        CHECK(flag == (INHERITS[i].ok ? INHERITS[i].want : !INHERITS[i].want), INHERITS[i].why);
    }
    for (i = 0; i < sizeof(NAMES) / sizeof(NAMES[0]); i++) {
        CssPropertyNames got = { NULL, 0 };
        bool ok = css_prelude_property_names(NAMES[i].s, strlen(NAMES[i].s), &got);

        CHECK(ok == NAMES[i].ok, NAMES[i].why);
        CHECK(!ok || got.n == NAMES[i].n, NAMES[i].why);
        /* A false answer allocates NOTHING, which is the entry's own contract and the one thing a leak here
           would be. Freeing an untouched list is a no-op, so this asserts it rather than relying on it. */
        CHECK(ok || (got.v == NULL && got.n == 0),
              "a refused `<custom-property-name>#` prelude left a list allocated");
        css_property_names_free(&got);
    }
}

/* FILE API §6.3 Packaging data — the one part of the FileReader that is a PURE FUNCTION of four values, and
 * therefore the part a fixture can hold to a known answer without an event loop under it. The event ORDER
 * §6.2 produces is exercised by wpt/FileAPI/reading-data-section; what is checked here is what those tests
 * cannot separate from the sequence — the BYTES each of §6.3's four arms returns.
 *
 * The rows are the standard's own switch, and the two that are not obvious carry their reason:
 * the empty-mimeType Data URL is `application/octet-stream` because §6.3's own text flags that step as
 * unspecified ("Better specify how the DataURL is generated. [Issue #104]") and the corpus pins the
 * interoperable answer; and the BOM row is Encoding §6.1 Legacy hooks for standards' `decode`, whose BOM
 * sniff overrules the label and the charset parameter alike. */
static void file_reader_package_selftest(JSContext *ctx)
{
    static const char UTF16BE_H[] = { (char)0xFE, (char)0xFF, 0x00, 'h' };
    static const char WIN1252_EURO[] = { (char)0x80 };
    static const char UTF8_HELLO[] = { 'h', 'e', 'l', 'l', (char)0xC3, (char)0xB6 };
    static const char RAW[] = { 0x00, 'A', (char)0xFF };
    static const struct {
        const char  *bytes;
        size_t       len;
        FileReadType type;
        const char  *mime;
        const char  *label;
        /* THE ANSWER AND ITS LENGTH, AS ONE `WANT(...)` — never two values a reader can disagree about.
           The length is here at all only because the BinaryString row's answer carries an EMBEDDED NUL, so
           strlen cannot serve; written as a NUMBER BESIDE the literal it is a value that is true when written
           and can simply be wrong, and it WAS: `application/octet-stream` is 24 bytes and both rows that
           substitute it were hand-counted as though it were 23. The row with a real mimeType passed, so the
           two wrong numbers read as a defect in the empty-mimeType SUBSTITUTION rather than in the fixture.
           `sizeof(literal) - 1` is computed by the compiler, counts the NUL-carrying row correctly, and
           cannot be miscounted — so the class of error goes with the numbers. */
        const char  *want;      /* the answer, as its UTF-8 bytes */
        size_t       want_len;
        const char  *why;
    } ROWS[] = {
#define WANT(s) s, sizeof(s) - 1
        { "TEST", 4, FILE_READ_DATA_URL, "text/plain", NULL,
          WANT("data:text/plain;base64,VEVTVA=="),
          "§6.3's DataURL arm uses mimeType when it is not the empty string" },
        { "TEST", 4, FILE_READ_DATA_URL, "", NULL,
          WANT("data:application/octet-stream;base64,VEVTVA=="),
          "§6.3's DataURL arm with no media type is the interoperable application/octet-stream (Issue #104)" },
        { "", 0, FILE_READ_DATA_URL, "", NULL,
          WANT("data:application/octet-stream;base64,"),
          "§6.3's DataURL arm over an empty byte sequence is the header and nothing after it" },
        { UTF8_HELLO, sizeof UTF8_HELLO, FILE_READ_TEXT, "", NULL,
          WANT("hell\xc3\xb6"),
          "§6.3's Text arm step 4: no label and no charset parameter is UTF-8" },
        { WIN1252_EURO, sizeof WIN1252_EURO, FILE_READ_TEXT, "", "windows-1252",
          WANT("\xe2\x82\xac"),
          "§6.3's Text arm step 2: the encodingLabel decides, and 0x80 in windows-1252 is U+20AC" },
        { WIN1252_EURO, sizeof WIN1252_EURO, FILE_READ_TEXT, "text/plain;charset=windows-1252", NULL,
          WANT("\xe2\x82\xac"),
          "§6.3's Text arm step 3: with no label the mimeType's charset parameter decides" },
        { WIN1252_EURO, sizeof WIN1252_EURO, FILE_READ_TEXT, "text/plain;charset=windows-1252", "UTF-8",
          WANT("\xef\xbf\xbd"),
          "§6.3's Text arm step 2 beats step 3: an explicit label overrides the type's charset" },
        { UTF16BE_H, sizeof UTF16BE_H, FILE_READ_TEXT, "text/plain;charset=windows-1252", NULL,
          WANT("h"),
          "Encoding §6.1's decode: the BOM overrules the charset parameter the row above obeyed" },
        { RAW, sizeof RAW, FILE_READ_BINARY_STRING, "", NULL,
          WANT("\x00" "A\xc3\xbf"),
          "§6.3's BinaryString arm: every byte is one code unit of equal value, NUL and 0xFF included" },
#undef WANT
    };
    size_t i;

    for (i = 0; i < sizeof ROWS / sizeof ROWS[0]; i++) {
        JSValue v = file_reader_package_data(ctx, ROWS[i].bytes, ROWS[i].len, ROWS[i].type,
                                             ROWS[i].mime, ROWS[i].label);
        size_t n = 0;
        const char *got;

        CHECK(!JS_IsException(v), ROWS[i].why);
        got = JS_ToCStringLen(ctx, &n, v);
        CHECK(got != NULL, ROWS[i].why);
        /* TWO ASSERTS AND NOT ONE CONJUNCTION. The abort prints the CONDITION, so a length that disagrees and
           bytes that disagree have to be different sentences or the report cannot say which — and this row's
           first failure was a wrong length read as wrong bytes, which pointed at the arm instead of at the
           fixture. Length FIRST, which is also what makes the memcmp below safe to write over `n`. */
        CHECK(n == ROWS[i].want_len, ROWS[i].why);
        CHECK(!memcmp(got, ROWS[i].want, n), ROWS[i].why);
        JS_FreeCString(ctx, got);
        JS_FreeValue(ctx, v);
    }
    {
        /* §6.3's ArrayBuffer arm: "Return a new ArrayBuffer whose contents are bytes." Its own row, because
           the answer is not a string and cannot be compared as one — which is the whole of what makes it the
           one arm the source overlay must not wrap. */
        JSValue v = file_reader_package_data(ctx, "TEST", 4, FILE_READ_ARRAY_BUFFER, "", NULL);
        size_t n = 0;
        const uint8_t *p = JS_GetArrayBuffer(ctx, &n, v);

        CHECK(p != NULL && n == 4 && !memcmp(p, "TEST", 4),
              "§6.3's ArrayBuffer arm did not answer with an ArrayBuffer over the byte sequence it was given");
        JS_FreeValue(ctx, v);
    }
}

/* §9.3.3's TWO ATTACKER SOURCES AND §Attacker-sources' FORGEABLE/UNFORGEABLE RULE, exercised where the rule is
 * DECIDED rather than through a page. The rule is one sentence with two verdicts and they are reached by two
 * different mechanisms, so a fixture that only drove a bundle would prove whichever of them that bundle wrote.
 *
 * THE UNFORGEABLE HALF IS A PIN. `e.origin === "https://trusted.test"` pins the value on its true arm
 * (decide.c's concretize-on-pin), and a pin on a source whose value the browser STAMPS is a demand no
 * cross-document attacker can meet — HTML §9.3.2.2 "User agents": "the integrity of this API is based on the
 * inability for scripts of one origin to post arbitrary events … to objects in other origins".
 * THE FORGEABLE HALF NEEDS NOTHING HERE, and that is the point of splitting them rather than testing a list of
 * string builtins: `e.origin.startsWith("https://trusted")` composes a predicate that pins NOTHING
 * (concolic_cmp answers OPCMP_NONE), so it never reaches the rule and the search proceeds — which is the
 * correct verdict, arrived at by the pin's own semantics.
 * BOTH SPELLINGS OF THE DEMAND ARE ONE DEMAND, which is the half a src-keyed rule would have missed:
 * `String(e.origin) === X` pins a DERIVED identity, and the attacker's principal is what both are about. */
/* ---- BEGIN tree-construction-write lane: HTML §13.2.6's DOM writes go through solver/dom_cow.h ------------
 *
 * §13.2.6.1 "Creating and inserting nodes"' "insert a character" step 3 — "If there is a Text node immediately
 * before insertionLocation, then append data to that Text node's data" — was a `lexbor_str_append` straight
 * into the node's own storage, with no interception point at any version, and it is what produces most of a
 * page's text. It goes through this engine's writer now, and the risk that change carries is not subtle: a
 * merge that appends the wrong bytes silently corrupts every document, and every gate in this repo would stay
 * green because the tree SHAPE is unaffected. So the fixture is over the BYTES.
 *
 * THE FIXTURE FORCES MULTIPLE CHARACTER TOKENS INTO ONE Text NODE, which is the only way to reach step 3 at
 * all: a character reference is tokenized as its own run, so `a&amp;b` is three character tokens and one Text
 * node, and `&#65;&#66;` is two references that must coalesce to `AB`. A single-token text would take step 3's
 * "Otherwise" arm and never merge. */
static void tree_construction_write_selftest(void)
{
    static const char *SRC = "<html><body><p>a&amp;b &#65;&#66; c</p></body></html>";
    static const char *WANT = "a&b AB c";
    lxb_html_document_t *dom = dom_document_create();
    lxb_dom_node_t *p, *text;
    lxb_dom_character_data_t *cd;
    size_t want_len = strlen(WANT);

    CHECK(dom != NULL, "the tree-construction fixture could not create a document");
    /* THE INSTALL IS ASSERTED FROM OUTSIDE THE PARSE, because inside it every §13.2.6 write has already
       happened: a table that was still lexbor's would have produced a byte-identical tree here (the default
       IS the vendor's behaviour), so the equality below would pass while nothing this lane built had run. */
    html_parse_document(dom, (const lxb_char_t *)SRC, strlen(SRC));
    CHECK(dom_cow_owns_tree_construction(),
          "§13.2.6's DOM writes are not this engine's after a parse — html_parse_new_parser installs them and "
          "the whole fixture below would be measuring lexbor's own default instead");

    p = lxb_dom_interface_node(lxb_html_document_body_element(dom))->first_child;
    CHECK(p != NULL && p->type == LXB_DOM_NODE_TYPE_ELEMENT,
          "the tree-construction fixture's <p> was not parsed into body");
    text = p->first_child;
    CHECK(text != NULL && text->type == LXB_DOM_NODE_TYPE_TEXT,
          "the tree-construction fixture's <p> has no Text child — §13.2.6.1's insert-a-character built none");
    /* ONE Text NODE, NOT FIVE. Coalescing is step 3 itself: if the routed append_data ever failed to write
       into the EXISTING node, the "Otherwise" arm would create a sibling per token and this would be false
       with every byte still present somewhere in the tree. */
    CHECK(text->next == NULL,
          "§13.2.6.1's insert-a-character did not coalesce its character tokens into one Text node — the "
          "routed append_data is creating a node per token instead of appending to the previous one");
    cd = lxb_dom_interface_character_data(text);
    CHECK(cd->data.length == want_len && !memcmp(cd->data.data, WANT, want_len),
          "§13.2.6.1's character merge produced the wrong bytes — the routed append_data must be byte-identical "
          "to the `lexbor_str_append` it replaced, or every parsed document's text is silently corrupt");

    /* AND THE ENTRY POINT ITSELF, on the node the parse just built. Capture is off in this harness, so what
       this exercises is the append half and its four DCHECKs; the delta half is the same kind-3 record
       dom_cow_set_text writes, whose unapply replaces the node's whole data with the captured baseline and so
       truncates the appended tail with the node's identity and position untouched. */
    dom_cow_append_text_data(text, "!!", 2);
    CHECK(cd->data.length == want_len + 2 && !memcmp(cd->data.data + want_len, "!!", 2),
          "dom_cow_append_text_data did not append to the Text node's data — §13.2.6.1 step 3 appends, and a "
          "write that replaced instead would drop every character token that came before it");
    CHECK(cd->data.data[want_len + 2] == 0x00,
          "dom_cow_append_text_data left the Text node's storage unterminated — lexbor's own readers take the "
          "NUL as well as the length, so a page's text would run into whatever follows it in the arena");
    dom_document_destroy(dom);
}
/* ---- END tree-construction-write lane -------------------------------------------------------------------- */

static void message_source_selftest(void)
{
    const char *kind = NULL, *enc;
    char prefix = 1;

    /* THE DECLARATION, BOTH HALVES. A mechanism and an address component that disagree is a reproduction
       nobody can build from, and `cross-document-message` is the one mechanism whose payload rides no address
       at all: the attacker holds a second document open beside the victim rather than writing its URL. */
    CHECK(concolic_source_delivery(MESSAGE_ORIGIN_SRC, &kind, &prefix) && kind
          && !strcmp(kind, "cross-document-message") && prefix == 0,
          "§9.3.3's `message.origin` does not declare the cross-document-message delivery — every @S finding "
          "rooted there would report as an exploit no navigation reaches, which is the silence §S(d)'s "
          "reproduction envelope reserves for a source no component carries");
    kind = NULL; prefix = 1;
    CHECK(concolic_source_delivery(MESSAGE_DATA_SRC, &kind, &prefix) && kind
          && !strcmp(kind, "cross-document-message") && prefix == 0,
          "§9.3.3's `message.data` does not declare the cross-document-message delivery — the two members a "
          "handler reads are one mechanism, and a `data` finding that could not state it would be the exact "
          "envelope-less PoC an orphan drive of the same handler already produced");
    /* AN EMPTY ENCODE SET IS A MEASUREMENT, AND ONLY THE DECLARED/UNDECLARED PAIR TELLS IT FROM AN ABSENT ROW
       — `concolic_source_encodes` answers NULL for a source that declared no delivery at all, and "" for one
       that declared a delivery which transforms nothing. §9.3.3 step 7's StructuredSerializeWithTransfer and
       step 8.4's StructuredDeserializeWithTransfer round-trip a string unchanged, which is why a postMessage
       breakout reproduces where the same candidate through a fragment dies on `<`. */
    enc = concolic_source_encodes(MESSAGE_DATA_SRC);
    CHECK(enc != NULL && *enc == 0,
          "§9.3.3's structured clone was declared to percent-encode the attacker's bytes, or to declare "
          "nothing at all — the first is false of steps 7 and 8.4 and the second is the shape a report reads "
          "as `no component carries these bytes`");

    /* A FLOW THAT HAS NARROWED NOTHING HAS DEMANDED NOTHING. Asserted first because every check below is a
       transition from this state, and a rule that answered 1 here would suppress every @S finding there is. */
    CHECK(!concolic_principal_pinned(),
          "the principal rule answered `pinned` for a constraint that holds nothing — every @S search would "
          "be suppressed at detection and the tool would emit no PoC at all");
    /* A PIN ON A SOURCE THE ATTACKER WRITES IS NOT A DEMAND ON A PRINCIPAL. This is the direction that must
       never move: `location.hash === "admin"` is the ordinary solved case, and a rule that caught it would
       silently delete the findings this half of the tool exists for. */
    concolic_pin("{location.hash}", "location.hash", "admin");
    CHECK(!concolic_principal_pinned(),
          "the principal rule read an ordinary equality pin as a demand on an attacker's own principal — a "
          "value the attacker WRITES is solved, and suppressing it drops a real PoC");
    /* …AND A PIN ROOTED AT THE PRINCIPAL IS ONE, in the spelling every bundle writes. */
    concolic_pin(MESSAGE_ORIGIN_SRC, MESSAGE_ORIGIN_SRC, "https://trusted.test");
    CHECK(concolic_principal_pinned(),
          "an exact equality against `event.origin` did not register as a demand on the attacker's principal "
          "— §Attacker-sources makes that check unsatisfiable cross-origin, so a PoC taken off this flow is "
          "one that cannot be delivered");
    concolic_clear_pins();
    CHECK(!concolic_principal_pinned(),
          "a demand survived the constraint being cleared — the rule reads the RUNNING flow's own narrowing, "
          "so one flow's origin check would suppress every other flow's findings");
    /* THE DERIVED SPELLING IS THE SAME DEMAND. `String(e.origin) === X` pins the derived identity and not the
       source, and a rule keyed by the pinned value's own name would answer `unpinned` for it — which is a
       false PoC for one of the two most common ways a real handler writes this check. */
    concolic_pin("String(" MESSAGE_ORIGIN_SHAPE ")", MESSAGE_ORIGIN_SRC, "https://trusted.test");
    CHECK(concolic_principal_pinned(),
          "an equality against a DERIVATION of `event.origin` did not register as a demand on the principal — "
          "the demand is about whose bytes these are, which no derivation changes, and that is why the pin "
          "records its ROOT beside its identity");
    concolic_clear_pins();
}

int main(int argc, char **argv) {
    JSRuntime *rt;
    trusted_types_selftest();
    policy_container_selftest();
    /* BEFORE the CSP element matching, because that check's hash arm is this primitive: a failure here would
       otherwise be reported as a CSP verdict being wrong. */
    secure_hash_selftest();
    csp_element_matching_selftest();
    csp_url_matching_selftest();
    document_policy_selftest();
    css_property_grammar_selftest();
    xml_char_selftest();   /* XML §2.2's [2] Char, §2.3's [3] S, and §2.11's line-break normalization */
    rt = JS_NewRuntime();
    JS_SetMaxStackSize(rt, 4 * 1024 * 1024);   /* align quickjs's overflow check with the emcc 8MB wasm stack */
    JSContext *ctx = JS_NewContext(rt);

    concolic_init(ctx);
    flow_registry_init("fixture");   /* one document in this fixture; the world namespace is named by it */
    world_registry_selftest(ctx);   /* the peer half: worlds minted as if by another document */
    flow_job_selftest(ctx);         /* §8.1.7's two queues, §7.5.10 step 7, and the fork's copy-on-nothing */
    sort_merge_selftest(ctx);       /* 23.1.3.30.1 step 4 rests per element, and both schedules agree */
    concat_keyed_selftest(ctx);     /* §23.1.3.2's keyed walk rests per request, declaring no yield of its own */
    endpoint_init();
    solve_init(ctx);

    /* BASELINE setup (mark 0): the globals here must NOT be captured, so install the COW hook AFTER.
       THIS FIXTURE'S DOCUMENT IS ITS OWN TOP-LEVEL TRAVERSABLE, so §8.1.3.1's top-level creation URL is the
       address it is installed at below — and `https:` makes it a SECURE CONTEXT, which is what a real bundle
       runs in and therefore what the fixture must exercise. */
    tf_agent_init(ctx, "https://x.test", "https://x.test/p");
    navigable_set_realm_builder(tf_child_realm);
    int min_doc = arg_has(argc, argv, "--min");   /* fast per-change memory gate: the minimal clone/COW doc */
    /* THE TWO SESSIONS OF THE CROSS-SESSION ROUND TRIP, one per invocation, because that is what a session
       boundary IS: the first writes its residue to this host's store and the process ends, the second starts
       from nothing but that document. Doing both inside one process would leave the endpoint surface, the sink
       searches and the world namespace of the first standing behind the second, so "the resumed session found
       it" and "the previous session had already found it" would be the same observation. */
    const char *cold_park_path = arg_val(argc, argv, "--cold-park");
    const char *cold_resume_path = arg_val(argc, argv, "--cold-resume");
    int cold_doc = (cold_park_path || cold_resume_path) ? 1 : 0;
    char *cold_residue = NULL;
    /* ONE OF THE TWO PER INVOCATION. The product's steady state is a session that resumes a residue AND
       re-parks what it did not finish, so this is a limit of the FIXTURE's verdict and not of the tier: the
       probe table names one document per run, and a session doing both would have to carry two sets of rows
       under one answer. Said as a refusal rather than left to produce a half-checked run. */
    DCHECK(!(cold_park_path && cold_resume_path),
           "this host was asked to park AND to resume in one invocation — its probe table answers for one "
           "session, so the two halves of the round trip are two runs of this binary");
    DCHECK(!(cold_doc && min_doc),
           "the cold round trip was asked for over the minimal document — the park refuses a frontier holding "
           "a foreign world's segment and that document opens child navigables, so this run would abort in "
           "cold_park naming the cross-instance park rather than measuring the tier");
    const char *doc = cold_doc ? HTML_COLD : (min_doc ? HTML_MIN : HTML);
    /* WHAT THIS INVOCATION IS, stated ONCE and read by the probe table — see it for why the document itself is
       what decides which rows this run carries. */
    g_doc = doc;
    g_probe_ctx = ctx;
    g_sess = cold_park_path ? SESS_PARK : cold_resume_path ? SESS_RESUME : SESS_EXPLORE;
    lxb_html_document_t *dom = dom_document_create();
    html_parse_document(dom, (const lxb_char_t *)doc, strlen(doc));
    g_body = lxb_dom_interface_element(lxb_html_document_body_element(dom));   /* the DOM sink's target element */
    {
        /* The fixture built this document, so the navigable's name is the initial "" and is known. */
        /* §7.5.1's OPENER POLICY ROW — §7.1.3's initial value, for the same reason the agent's is: this
           fixture's document came from no response. */
        JSValue root_proxy = window_proxy_new_self(ctx, world_local_doc(), "", OPENER_POLICY_UNSAFE_NONE);
        CHECK(!JS_IsException(root_proxy), "the root navigable's WindowProxy could not be allocated");
        /* NULL: this fixture's document is a C string literal, so it came from no response and has no
           header-borne policy. That is a fact about it, not a gap. */
        /* AND AN EMPTY §7.1.5 ACTIVE SANDBOXING FLAG SET, from the same fact and the same sentence: the two
           halves §7.4.5 unions are this navigable's CREATION sandboxing flags — empty, because a top-level
           traversable has no embedder element and its POPUP sandboxing flag set begins empty — and the
           CSP-derived flags of the policy above, which there is none of. Not a placeholder: an unsandboxed
           document's set IS empty, and this fixture's is. */
        /* CSP §2.2.2's SELF-ORIGIN, which this fixture's document has even though it has no policy: a CSP
           list carries one whether or not it holds any policies, and `'self'` in a policy this fixture builds
           later is measured against it. It is this document's own address's origin, because this document is
           its own response. */
        tf_realm_install(ctx, dom, "https://x.test/p", "https://x.test", NULL, "https://x.test", 0,
                         world_local_doc(), root_proxy);
        JS_FreeValue(ctx, root_proxy);
    }

    /* INDEXED DATABASE'S PUT/GET ROUND TRIP, at the BASELINE and before the time-travel hooks — the same
       position core/file/file_system.c's two roots are built at, and for the same reason: a record written
       here belongs to the pre-boot baseline rather than to whichever flow happened to run first. */
    idb_store_selftest(ctx);

    /* FILE API §6.3's four arms, over known bytes. It needs only a realm — see the function for why this
       half of the FileReader is the half a C fixture can hold to an answer at all. */
    file_reader_package_selftest(ctx);
    /* AFTER the platform init above, because the two rows it checks are declared by window_message_init. */
    message_source_selftest();   /* §9.3.3's sources, and the unforgeable-origin rule that decides their findings */
    tree_construction_write_selftest();   /* §13.2.6's DOM writes, and the character merge's bytes */

    /* The two hook SETS the solver owns, each declared by its own component. They were struct literals here
       and again in test_forced.c, and the pair had drifted. */
    cow_install_time_travel_hooks(engine_gen_fork);
    concolic_install_hooks();
    concolic_install_source_overlay();   /* a SOLVER host: attacker-controlled values are symbolic sources */
    /* The surface is installed, so every member the platform has is declared — a declaration from here on is a
       per-wrapper or per-flow mint, and that is what the pool asserts against. */
    idl_args_seal();

    /* The trusted host's half: what it is owed, and how it pays. */
    engine_set_provider(fixture_provide);

    DocScripts scripts = document_exec_scripts(dom);   /* each <script> its own program body — no concat */
    /* THE RESIDUE THIS SESSION IS CONTINUING, read off the shelf before the scheduler is seeded — because the
       choice between it and a boot flow is made once, inside engine_sched_begin, and a residue that arrived
       after that point would be a second seeding of the same document. */
    if (cold_resume_path) cold_residue = tf_park_load(cold_resume_path);
    /* WHEN THIS ENGINE LEAVES MEMORY — ONE seam, and the two sessions that install it answer it about different
       things because they are measuring different things. The --cold-park session's moment is a statement about
       the RESIDUE (a candidate standing on a segment: every arm of the recipe grammar in one member). An
       EXPLORING session's is a statement about the DOCUMENT — every statement it makes answered — because its
       frontier is unbounded and "it drained" is a completion condition no document owes anybody. The --cold-resume
       session installs neither: its document drains, and its rows are about what the tier REBUILT. */
    if (cold_park_path)            engine_set_park_hook(fixture_want_park);
    else if (!cold_resume_path)    engine_set_park_hook(fixture_have_answers);
    engine_run(ctx, scripts.bodies, scripts.srcs, scripts.types, scripts.els, scripts.n, cold_residue);   /* @H + @S detection */
    /* No verify call: the candidate re-fires are FLOWS on the same frontier, so engine_run already ran them. */
    doc_scripts_free(&scripts);
    dom_document_destroy(dom);
    free(cold_residue); cold_residue = NULL;   /* engine_sched_begin rebuilt from it; the text was borrowed */

    /* ─── THE COLD TIER'S TWO ENDS, EACH REPORTED PER RECORD KIND ────────────────────────────────────────────
       A total cannot say which ARMS ran, and the arms are the whole question: a residue of nothing but 'f'
       records reads back without touching the segment rebuild, park_unhex or the sink-class re-bind, and
       reports the same count as one that touches all three. Both lines carry the same three fields for the
       same reason, so the two ends of one round trip can be read against each other by eye. A fourth,
       `probes`, is gone with the record kind it counted ('d', an engine-seeded discovery probe): active
       discovery is the trusted zone's again, so the arm cannot run and a field reporting 0 forever would say
       it merely had not. */
    /* THE TWO CENSUSES ARE THE FIXTURE'S OWN RECORD (g_cp/g_cr) rather than two locals, because the probe rows
       that are statements ABOUT them are computed where the table is. */
    /* ASKED OF THE ENGINE, NOT OF THE ARGUMENTS. Every session that PARKED reports its residue, and whether it
       also has a FILE to put it in is a different question — the exploring sessions park too now, and their
       residue rides the result document's `_park` array exactly as the extension's does. A census printed only
       for the invocation that named a path would have gone silent for them. */
    if (engine_frontier_paged()) {
        const char *recipes = cold_park_recipes();
        cold_parked(&g_cp);
        /* THE RESIDUE GOES TO THE SHELF BEFORE THE TEARDOWN RUNS. flow_registry_free frees the park document
           with the frontier it belongs to, so a store written after it would be written from freed memory —
           and the residue is the only remaining copy of every flow in it. */
        if (cold_park_path) tf_park_store(cold_park_path, recipes);
        printf("@COLDPARK {\"records\":%ld,\"segs\":%ld,\"flows\":%ld,\"cands\":%ld,\"orphans\":%ld,"
               "\"bytes\":%zu,\"store\":\"%s\"}\n",
               cold_park_records(), g_cp.segs, g_cp.flows, g_cp.cands, g_cp.orphans, strlen(recipes),
               cold_park_path ? cold_park_path : "-");
    }
    if (cold_resume_path) {
        long met = 0, unmet = 0;

        cold_resumed(&g_cr);
        /* THE REBUILD AND WHAT BECAME OF IT ON ONE LINE. `orphans` is what the residue named; `met` is how many
           of those waits a take satisfied and `unmet` how many finished with nothing — the pair says whether
           the locator found anything, which the rebuild count alone cannot. */
        engine_orphan_claims(&met, &unmet);
        printf("@COLDRESUME {\"segs\":%ld,\"flows\":%ld,\"cands\":%ld,\"orphans\":%ld,"
               "\"orphansMet\":%ld,\"orphansUnmet\":%ld}\n",
               g_cr.segs, g_cr.flows, g_cr.cands, g_cr.orphans, met, unmet);
    }

    /* ONE result document — both surfaces and the scheduler's interleave count, serialized DIRECTLY from the
       C findings (no JS-object round-trip). The host does one JSON.parse of this line and relays it; it used
       to be two lines here, which meant whoever consumed them assembled the document, and assembling is
       structure. The assertions below read the same string, so they cover the composed shape. */
    char *js = result_json(ctx);
    CHECK(js, "the result document could not be rendered — this fixture's whole verdict is a function of it, so "
              "there is nothing to report and nothing to assert");
    printf("@RESULT %s\n", js);
    /* THE VERDICT, so a 0 row is FAIL: the run is over and the row will not be reached. */
    int h_ok = probes_report(js, true);

    /* THE SENTENCE NAMES WHAT THIS INVOCATION MEASURED. A cold session runs none of the @H/@S rows, so
       reporting their verdict over it would be a claim about a program it did not run — the same defect the
       probe key exists to prevent, one table above. */
    if (cold_doc)
        printf("%s\n", h_ok
            ? (cold_park_path
                ? "PASS: the frontier was written to this host's cold-tier store as recipes — @S candidate "
                  "sessions and the arms still exploring, each standing on the decision segments it forked at, "
                  "which is every record kind this document can produce (see HTML_COLD for the one that needs a "
                  "capability nothing here has) — resume it with --cold-resume over the same path"
                : "PASS: the parked residue was rebuilt into the ONE frontier, and a resumed @S candidate "
                  "replayed its recorded arms back to the sink it was suspended in front of and FIRED there")
            : "FAIL: the cross-session round trip did not exercise the tier — read the 0 rows above and the "
              "@COLDPARK/@COLDRESUME census beside them");
    else
    /* THE SENTENCE NAMES NO STATEMENT, and that is the correction the derived selector forces on it. It read
       "@S eval + innerHTML + location + a REAL Location source", which is the FULL document's list — the
       minimal one has no `location.hash` sink at all, so under --min the PASS line was naming two rows that
       were never selected. What is true of both is the table: every statement THIS document makes, answered. */
    printf("%s\n", h_ok
        ? "PASS: every statement this document makes has been answered — the @H row above is which, and the "
          "residue left over is real and parked (@COLDPARK, and `_park` in the result document)"
        : "FAIL: a statement this document makes went unanswered — the 0 rows above name which");

    free(js);
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
    animation_frame_free(ctx);
    event_loop_free(ctx);   /* §8.1.7's own record — the virtual clock and the moments beside it */
    /* §8.1.7.5's rejection list is a row on core/platform.h's release column now, run by the
       platform_agent_free above — see main.c's teardown for what having it here cost the host that did not. */
    abort_free(ctx);
    observable_free(ctx);
    document_free(ctx);   /* the window reference the lifecycle holds */
    /* THE WHOLE DOM GROUP — element_free's cascade, the <iframe> element and GEOMETRY INTERFACES §3/§4 — is a
       set of ROWS on core/platform.h's release column now, run by the platform_agent_free above. This LINE
       stays: document_free releases a REALM's record, not the agent's. The component's other half —
       document_agent_free — is a row on that column, and the two are not ordered against each other. See
       main.c's teardown. */
    realm_intrinsics_free();   /* the DECLARATIONS are the agent's; each realm's prototypes went with it */
    report_exception_free(ctx);
    event_free(ctx);
    headers_free(ctx);    /* Headers.prototype and the name it interned */
    url_free(ctx);
    usp_free(ctx);
    transform_stream_free(ctx);
    writable_stream_free(ctx);
    queuing_strategy_free(ctx);
    readable_stream_free(ctx);
    blob_free(ctx);
    encoding_free(ctx);
    text_stream_free(ctx);
    form_data_free(ctx);        /* URLSearchParams.prototype */
    response_free(ctx);
    request_free(ctx);   /* Response.prototype — one object, held for the runtime's life */
    navigable_free(ctx);
    /* navigator (and Permissions §6 + §3.2's store with it), storage_manager and screen are ROWS on
       core/platform.h's release column now, run by the platform_agent_free above. §3.2's store is two live
       Arrays reached only through navigator_free, and the host that had no such line — the WPT runner —
       leaked both in every file it ran. A teardown each host writes out by hand is a teardown some host is
       missing a row of, and nothing reports it but the runtime's own leak walk, after the fact. */
    /* §7.2.4's Location is a ROW on that column too — it holds TWO CLAIMS in solver/concolic.c's source
       registry, whose emptiness concolic_free asserts, and a claim placed by three hand-written lists is a
       claim placed by nothing. */
    session_history_free();
    history_free();
    navigation_free(ctx);              /* HTML §7.2.6 the navigation API */
    navigation_history_entry_free(ctx);
    window_free(ctx);
    remote_object_free(ctx);
    window_proxy_free(ctx);   /* the shared §7.2.5.1 prototype every proxy is chained to */
    /* THE SOLVER'S OWN LIST, UNDONE — one call, in solver/engine.h, for the reason the platform's is one call:
       these six lines were hand-copied into three hosts and had already drifted three ways. See that header. */
    solver_agent_free(ctx);
    /* AFTER THE FRONTIER, because a flow parked inside an IDL member reads this pool at its teardown — this
       line was above navigable_free, where the pool was already gone by the time flow_registry_free released
       the residue. idl_args_free asserts the ordering. */
    idl_args_free(ctx);   /* the dictionary member atoms the declaration pool interned */
    JS_RunGC(rt);   /* collect flow-local garbage from the runs before teardown */
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    /* AFTER JS_FreeRuntime, and it is the one teardown line whose ORDER is part of its meaning: what
       this releases is part of a step DEFINITION, which JS_RegisterStepDef borrows and requires to
       outlive the runtime — JS_FreeRuntime's own [stepleak] report reads `def->steps` to name each
       unfinished machine by the step it rests at. The IDL pool's BLOCKS are the same obligation: each holds
       the definition the runtime borrowed. */
    idl_args_pool_free();
    idl_async_iter_free();
    return h_ok ? 0 : 1;
}
