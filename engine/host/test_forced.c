/* End-to-end proof of the rebuilt frame-agnostic forced-execution core: a single concolic branch must explore
 * BOTH arms via the dispatch loop, with the fork expressed purely as a decision-vector sibling (no OP_if
 * rewind, no frame snapshot). Runs standalone against clean upstream quickjs + the new solver components. */
#include "quickjs.h"
#include "check.h"
#include "solver/concolic.h"
#include "solver/decide.h"
#include "solver/flow.h"
#include "solver/world.h"
#include "core/frame/csp_source_list.h"
#include "core/frame/navigable.h"
#include "core/timing/event_loop.h"
#include "core/timing/timer.h"
#include "core/frame/window.h"
#include "core/frame/window_proxy.h"
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
    "</head><body><div id=cs1 style=\"margin-top: 4px\"></div><h1 id=dh>doch</h1>"
    "<script>var cfg = { admin: state.admin };"
    "var delObj = { k: 'keepVAL' };"   /* a shared BASELINE object; a forked flow will DELETE its k -> must revert per-flow */
    "var rx = { _f: 'base' };"   /* a reactive-framework style object: `flag` is an ACCESSOR backed by _f (Vue does exactly this) */
    "Object.defineProperty(rx, 'flag', { get: function(){ return this._f; }, set: function(v){ this._f = v; }, configurable: true });</script>"
    "<script>"
    "fetch('/api/u?uid=' + state.id);"   /* concolic query param -> uid carries {state}.id */
    "if (navigator.userAgent.indexOf('Chrome') >= 0) { fetch('/api/uafork?v=chrome'); } else { fetch('/api/uafork?v=other'); }"   /* THE UA GATE: navigator.userAgent is concolic with a real Chrome example, so the string method computes on the example AND the comparison forks -> BOTH arms' endpoints are learned */
    "if (navigator.maxTouchPoints > 0) { fetch('/api/touch?v=touch'); } else { fetch('/api/touch?v=mouse'); }"
    "if (screen.width < 768) { fetch('/api/layout?v=mobile'); } else { fetch('/api/layout?v=desktop'); }"
    "var ac0c = new AbortController(); var ac0 = ac0c.signal;"
    "ac0.addEventListener({ toString: function(){ var n = 0; for (var i = 0; i < 500; i++) { n += i; } return 'abort'; } }, function(){ fetch('/api/idlcoerce?v=coerced'); });"   /* addEventListener's `type` is a Web IDL DOMString: the toString is the PAGE's code and has a loop in it, so the machine parks on step_tostring_run and resumes at the exact stage — and the listener is registered under the string that call RETURNED, which the abort below proves by firing it */
    "ac0c.abort();"
    "var ac = new AbortController(); ac.signal.addEventListener('abort', function(e){ fetch('/api/aborted?v=' + (e.isTrusted && e.type === 'abort' && e.target === ac.signal ? 'fired' : 'wrong')); }); ac.abort();"   /* the controller's signal is the REAL state machine: abort() reads [[Signal]] as an internal slot and fires `abort`, whose listener runs as its own task on this flow */
    "var tsig = AbortSignal.timeout({ valueOf: function(){ var n = 0; for (var i = 0; i < 2000; i++) { n += i; } return n; } });"   /* [EnforceRange] unsigned long long is ToNumber on the PAGE's object: the loop inside valueOf preempts, so the timeout machine must suspend and resume at the exact stage it parked on */
    "if (tsig.aborted) { fetch('/api/deadline?v=expired'); } else { fetch('/api/deadline?v=live'); }"   /* a timeout's aborted flag is UNKNOWN, so both arms run and the fallback path's endpoint is learned too */   /* THE responsive gate: a bundle routes, hosts assets and often bases its API on this, so both arms must be reached */   /* the desktop-vs-touch gate, the same shape over a numeric member */
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
    /* §4.8.5's REMOVING STEPS: the element loses its navigable, and the proxy the page still holds stays the
       same object while reporting a destroyed one. */
    "document.body.removeChild(_if);"
    "_ifok = _ifok && _if.contentWindow === null && _cw.closed === true && _cw.name === '' &&"
    " _cw.self === _cw && _if.getAttribute('name') === 'fr';"
    "fetch('/api/iframenav?v=' + (_ifok ? 'ifnav' : 'wrong'));"

    /* WHAT IS NOT PROBED HERE, AND WHY IT IS NOT A CHOICE: an iframe with a REAL `src`. It would exercise the
       whole of §7.4 step 14 — the load job asks the host for the address, PARKS, resumes with the response and
       runs the child's own scripts — and the srcless probe above reaches none of that, because about:blank has
       no response to fetch. It was written, and it aborts: a fixture statement runs in EVERY flow, a src'd
       navigable materializes a realm rather than deferring it (its scripts are an observable), and a realm is
       never reclaimed — so this fixture's ~7000 flows ask for ~7000 child realms and the heap is gone at 4022.
       That is the ceiling child_document's CHECK names, and it is the next mechanism, not a probe to soften.
       The path is not unexercised meanwhile: `node engine/wpt.mjs html/browsers` runs hundreds of src'd
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
       them, which is the opposite quantifier from policy_allows and the one a copy of that loop would get
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
    CHECK(policy_allows(none, POLICY_INLINE_HANDLER), "no policy must permit an inline handler");
    CHECK(policy_allows(none, POLICY_EVAL), "no policy must permit eval");

    self_only = policy_container_new("script-src 'self'", self_origin, NULL);
    /* §S's own example: an inline onerror is DEAD under `script-src 'self'`, and so is a javascript: URL. A
       host source never permits inline execution — that is what 'unsafe-inline' is for. */
    CHECK(!policy_allows(self_only, POLICY_INLINE_HANDLER), "'self' must not permit an inline handler");
    CHECK(!policy_allows(self_only, POLICY_JAVASCRIPT_URL), "'self' must not permit a javascript: URL");
    CHECK(!policy_allows(self_only, POLICY_EVAL), "'self' must not permit eval");

    inline_ok = policy_container_new("default-src 'none'; script-src 'unsafe-inline'", self_origin, NULL);
    CHECK(policy_allows(inline_ok, POLICY_INLINE_HANDLER), "'unsafe-inline' must permit an inline handler");
    CHECK(!policy_allows(inline_ok, POLICY_EVAL), "'unsafe-inline' must not permit eval");

    /* CSP §6.1: a nonce source makes 'unsafe-inline' be IGNORED — the rule that makes adding a nonce to a
       legacy policy actually tighten it rather than widen it. A handler can carry no nonce, so it stays dead. */
    nonced = policy_container_new("script-src 'unsafe-inline' 'nonce-abc'", self_origin, NULL);
    CHECK(!policy_allows(nonced, POLICY_INLINE_SCRIPT), "a nonce source must make 'unsafe-inline' ignored");
    CHECK(!policy_allows(nonced, POLICY_INLINE_HANDLER), "a handler carries no nonce, so it stays blocked");

    /* CSP §6.1: a present `script-src` REPLACES `default-src` for scripts rather than adding to it. The first
       version of this parser OR'd every script-governing directive's sources into one flag set, and this is
       the case that exposes it — the handler came out ALLOWED because `default-src`'s 'unsafe-inline' survived
       a `script-src` that does not carry it. */
    {
        PolicyContainer *overridden = policy_container_new("default-src 'unsafe-inline'; script-src 'self'", self_origin, NULL);
        CHECK(!policy_allows(overridden, POLICY_INLINE_HANDLER),
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
        CHECK(!policy_allows(granular, POLICY_INLINE_HANDLER), "script-src-attr 'none' must kill a handler");
        CHECK(policy_allows(granular, POLICY_JAVASCRIPT_URL),
              "§6.8.2 maps a `navigation` inline check to script-src-elem, so script-src-attr must not touch it");
        CHECK(policy_allows(granular, POLICY_INLINE_SCRIPT),
              "script-src-attr must not govern a script ELEMENT — that is script-src-elem's fallback to script-src");
        CHECK(policy_allows(granular, POLICY_EVAL), "eval has no granular form and reads script-src");
        policy_container_free(granular);
    }
    /* ...and the same policy with the granular form that DOES govern a navigation kills it, which is what
       makes the line above a statement about which directive rather than about which answer. */
    {
        PolicyContainer *elem =
            policy_container_new("script-src 'unsafe-inline'; script-src-elem 'none'", self_origin, NULL);
        CHECK(!policy_allows(elem, POLICY_JAVASCRIPT_URL), "script-src-elem 'none' must kill a javascript: URL");
        CHECK(!policy_allows(elem, POLICY_INLINE_SCRIPT), "script-src-elem 'none' must kill a script element");
        CHECK(policy_allows(elem, POLICY_INLINE_HANDLER), "script-src-elem must not govern an event handler");
        policy_container_free(elem);
    }
    /* §6.7.3.2's OTHER two overrides of 'unsafe-inline', neither of which this file could answer before: a
       HASH source and 'strict-dynamic' both make the whole directive stop allowing all inline behavior. Every
       one of these policies used to ABORT the process — the old parser recorded any source expression it did
       not model and then DCHECKed on it, which is most of the real web. */
    {
        PolicyContainer *hashed =
            policy_container_new("script-src 'unsafe-inline' 'sha256-YWJj'", self_origin, NULL);
        CHECK(!policy_allows(hashed, POLICY_INLINE_SCRIPT), "a hash source must make 'unsafe-inline' ignored");
        CHECK(!policy_allows(hashed, POLICY_INLINE_HANDLER),
              "a hash source overrides 'unsafe-inline' for the WHOLE directive, handlers included");
        policy_container_free(hashed);
    }
    {
        /* `'sha1-…'` is not a hash-source: §2.3.1's hash-algorithm is exactly sha256/sha384/sha512, so this is
           an unrecognised expression, which §6.7.3.2 IGNORES rather than treats as an override. The two lines
           differ by the digest length alone and must not agree. */
        PolicyContainer *not_a_hash = policy_container_new("script-src 'unsafe-inline' 'sha1-YWJj'", self_origin, NULL);
        CHECK(policy_allows(not_a_hash, POLICY_INLINE_SCRIPT),
              "an expression outside the grammar must be ignored by §6.7.3.2, not read as a hash source");
        policy_container_free(not_a_hash);
    }
    {
        PolicyContainer *strict = policy_container_new("script-src 'unsafe-inline' 'strict-dynamic'", self_origin, NULL);
        CHECK(!policy_allows(strict, POLICY_INLINE_SCRIPT), "'strict-dynamic' must override 'unsafe-inline'");
        CHECK(!policy_allows(strict, POLICY_JAVASCRIPT_URL),
              "'strict-dynamic' covers the navigation type as well as script and script attribute");
        policy_container_free(strict);
    }
    /* HOST AND SCHEME SOURCES ARE NOT AN INLINE ANSWER AT ALL — §6.7.3.2 never looks at them — so a policy
       made of them permits exactly what its keywords permit. This is the shape almost every real policy has,
       and it is the one the old parser aborted on. */
    {
        PolicyContainer *hosts =
            policy_container_new("script-src https: https://*.example.com:443/a/b 'unsafe-inline'", self_origin, NULL);
        CHECK(policy_allows(hosts, POLICY_INLINE_HANDLER),
              "host and scheme sources are invisible to §6.7.3.2, so 'unsafe-inline' still allows all inline");
        CHECK(!policy_allows(hosts, POLICY_EVAL), "and none of them is 'unsafe-eval'");
        policy_container_free(hosts);
    }
    /* §2.2.1: within ONE policy a repeated directive is IGNORED, so the first wins... */
    {
        PolicyContainer *dup = policy_container_new("script-src 'unsafe-inline'; script-src 'self'", self_origin, NULL);
        CHECK(policy_allows(dup, POLICY_INLINE_HANDLER), "a repeated directive in one policy must be ignored");
        policy_container_free(dup);
    }
    /* ...and §2.2.1 lowercases the name before that containment test, so the repeat is a repeat however it is
       spelled — `script-SRC 'none'` and `ScRiPt-sRc 'none'` are the standard's own example of equivalence. */
    {
        PolicyContainer *cased = policy_container_new("SCRIPT-SRC 'unsafe-inline'; script-src 'none'", self_origin, NULL);
        CHECK(policy_allows(cased, POLICY_INLINE_HANDLER),
              "a directive name is matched ASCII case-insensitively, so the second one is the ignored repeat");
        policy_container_free(cased);
    }
    /* ...but §2.2's policy LIST is comma-delimited and enforced INDEPENDENTLY, so the very same two directives
       as two POLICIES must intersect instead. These two lines differ by one character and must not agree. */
    {
        PolicyContainer *list = policy_container_new("script-src 'unsafe-inline', script-src 'self'", self_origin, NULL);
        CHECK(!policy_allows(list, POLICY_INLINE_HANDLER),
              "policies in a list are enforced independently — a second policy can only NARROW");
        policy_container_free(list);
    }
    /* A policy that governs no script directive forbids nothing about scripts. */
    {
        PolicyContainer *unrelated = policy_container_new("img-src 'none'; frame-ancestors 'none'", self_origin, NULL);
        CHECK(policy_allows(unrelated, POLICY_INLINE_HANDLER), "img-src must not block a handler");
        CHECK(policy_allows(unrelated, POLICY_EVAL), "frame-ancestors must not block eval");
        policy_container_free(unrelated);
    }

    evals = policy_container_new("script-src 'unsafe-eval'", self_origin, NULL);
    CHECK(policy_allows(evals, POLICY_EVAL), "'unsafe-eval' must permit eval");
    CHECK(!policy_allows(evals, POLICY_INLINE_HANDLER), "'unsafe-eval' must not permit an inline handler");

    /* §7.4: the initial about:blank's container is a CLONE of its creator's — which is the whole of how a
       same-origin popup or iframe with no URL inherits CSP, and it is a deep copy so the child's answers do
       not move when the parent is navigated. */
    child = policy_container_clone(self_only);
    CHECK(!policy_allows(child, POLICY_INLINE_HANDLER), "a cloned container must carry the creator's policy");
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
    CHECK(!policy_allows(p, POLICY_INLINE_HANDLER),
          "two meta policies must INTERSECT — the second's 'self' forbids what the first's 'unsafe-inline' "
          "permits, and a scan that let the last one win would report a live handler on a page that blocks it");
    CHECK(!policy_allows(p, POLICY_EVAL), "neither meta policy carries 'unsafe-eval'");

    /* A page with no CSP at all is the overwhelmingly common one, and the scan must not invent a policy for
       it — an empty container that answered "blocked" would suppress every real finding on every such page. */
    plain = dom_document_create();
    html_parse_document(plain, (const lxb_char_t *)"<html><body></body></html>", 26);
    empty = document_policy_new(plain, NULL, self_origin);
    CHECK(policy_allows(empty, POLICY_INLINE_HANDLER), "a document with no meta CSP must permit everything");

    {
        /* §7.2.6's OTHER HALF. The response header and the `<meta>` policies are ONE LIST and every policy in
           it is enforced, so a header that forbids inline must still forbid it on a page whose meta permits it
           — which is exactly the page above. The engine's entry point used to drop the header, so this page
           reported a live inline handler that the real response kills. */
        PolicyContainer *hdr = document_policy_new(plain, "script-src 'self'", self_origin);
        PolicyContainer *both = document_policy_new(dom, "default-src 'unsafe-inline'", self_origin);
        CHECK(!policy_allows(hdr, POLICY_INLINE_HANDLER),
              "a header-borne policy must be enforced on a document whose tree declares none");
        CHECK(!policy_allows(both, POLICY_INLINE_HANDLER),
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
                   the peer above, so the load's suspend/resume runs end to end. `{body, csp}` is ONE answer
                   because a policy is a property of THE RESPONSE. The body is a DOCUMENT WITH A SCRIPT,
                   because the only thing that proves a navigation happened is the loaded document RUNNING. */
                static const char DOC[] =
                    "<!doctype html><html><head></head><body>"
                    "<script>fetch('/api/iframesrc?v=loaded');</script></body></html>";
                v = JS_NewObject(ctx);
                /* AS BYTES: §2.2.5's body is a byte sequence, and a Document is parsed from one. */
                JS_SetPropertyStr(ctx, v, "body",
                                  JS_NewArrayBufferCopy(ctx, (const uint8_t *)DOC, sizeof DOC - 1));
                JS_SetPropertyStr(ctx, v, "csp", JS_NULL);
            } else {
                v = JS_NewStringLen(ctx, tab + 1, (size_t)(end - tab - 1));
            }
            /* ONE ANSWER PER REQUEST. Answering inside a branch AND here answered twice, which the engine's own
               assert named at the site — a second answer would overwrite a value the asking machine may
               already have read. */
            /* This fixture stands in for the trusted zone and answers out of its own tables, so every answer
               it gives is a NORMAL completion — there is no peer program here to have thrown in one. */
            n += engine_host_answer(ctx, id, v, ENGINE_COMPLETION_NORMAL, ENGINE_ANSWER_HOST);
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
    concolic_pin("{reply}.id", "u-7");
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
    int s_eval = strstr(ss, "\"sink\":\"eval\"") && strstr(ss, "{state}.code") && strstr(ss, "';X9()//");
    /* THE SECOND §12 STATE, and it is the half that distinguishes a derivation from a renamed table: the same
       sink class, a different lexical state, and an escape no fixed list contained. The expected text is
       JSON-escaped because the escape IS a LineTerminator — §12.4 puts it outside the comment — so what the
       report carries is a backslash and an `n`, not a byte a payload table could have spelled. */
    int s_evalc = strstr(ss, "{state}.note") && strstr(ss, "\\nX9()");
    int s_html = strstr(ss, "\"sink\":\"innerHTML\"") && strstr(ss, "{state}.html") && strstr(ss, "<svg onload=X9()>");
    int s_url = strstr(ss, "\"sink\":\"location\"") && strstr(ss, "{state}.next") && strstr(ss, "javascript:X9()");
    /* THE SOURCE'S BROWSER TRANSFORM, asserted end to end: a breakout through the REAL Location, whose value
       the browser percent-encodes per the fragment set and prefixes with `#`. The apostrophe is not in that set,
       so `';X9()//` arrives intact and fires — and it fires through `'#';X9()//'`, the leading `#` included.
       Before the transform existed the payload was handed over raw, so this fired for the wrong reason and an
       HTML-context breakout through the same source would have fired too, which a browser would not. */
    int s_loc = strstr(ss, "\"source\":\"" LOCATION_HASH_SRC "\"") && strstr(ss, "';X9()//");
    /* THE NEGATIVE HALF, and it is an assertion about the REPORT, not about a missing line. The same source
       into an HTML sink must produce NO PoC — the fragment set encodes `<`, so every HTML candidate arrives as
       `%3C` and parses as text — and must still be REPORTED, as a parked search carrying the encode set that
       defeated it. Asserting only "no PoC" would also pass if the sink were never detected, which is the false
       negative this half exists to catch; asserting the parked entry says the sink WAS reached and searched. */
    int s_park = strstr(ss, "\"sink\":\"innerHTML\",\"source\":\"" LOCATION_HASH_SRC "\",\"search\":\"parked\"")
              && strstr(ss, "\"sourceEncodes\":\" \\\"<>`\"")
              && !strstr(ss, "\"source\":\"" LOCATION_HASH_SRC "\",\"poc\":\"<");
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
        { "floc-iso", floc_iso, "/api/floc", SESS_EXPLORE },
        { "ua", uafork_tt, "/api/uafork", SESS_EXPLORE },
        { "touch", touchfork_tt, "/api/touch", SESS_EXPLORE },
        { "layout", layoutfork_tt, "/api/layout", SESS_EXPLORE },
        { "abort", abortfire_tt, "/api/aborted", SESS_EXPLORE },
        { "deadline", deadline_tt, "/api/deadline", SESS_EXPLORE },
        { "idl", idlcoerce_tt, "/api/idlcoerce", SESS_EXPLORE },
        { "dom-idl", domidl_tt, "/api/protoid", SESS_EXPLORE },
        { "node-algo", nodealgo_tt, "/api/nodeconst", SESS_EXPLORE, nodealgo_why },
        { "pushfork", pushfork_tt, "/api/pushfork", SESS_EXPLORE },
        { "mapfork", mapfork_tt, "/api/mapfork", SESS_EXPLORE },
        { "fefork", fefork_tt, "/api/fefork", SESS_EXPLORE },
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
        { "iframe-nav", ifnav_tt, "/api/iframenav", SESS_EXPLORE },
        { "idb-open", idbopen_tt, "/api/idbopen", SESS_EXPLORE },
        { "idb-record", idbrec_tt, "/api/idbrec", SESS_EXPLORE },
        { "idb-record-taint", idbtaint_tt, "/api/idbrec", SESS_EXPLORE },
        { "loc-hash-param", lochash_tt, "/api/idbrec", SESS_EXPLORE },
        { "idb-index", idbidx_tt, "/api/idbidx", SESS_EXPLORE },
        { "idb-index-uniq", idbuniq_tt, "/api/idbuniq", SESS_EXPLORE },
        { "optiter", optiter_tt, "/api/optiter", SESS_EXPLORE },
        { "s-eval", s_eval, "state.code", SESS_EXPLORE },
        { "s-evalc", s_evalc, "state.note", SESS_EXPLORE },
        { "s-html", s_html, "state.html", SESS_EXPLORE },
        { "s-url", s_url, "state.next", SESS_EXPLORE },
        { "s-loc", s_loc, "location.hash", SESS_EXPLORE },
        { "s-park", s_park, "location.hash", SESS_EXPLORE },
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
           raised at SEED time. This is that premise: a candidate of that search DELIVERED its bytes to the
           write and the fragment encode set defeated it. Without it, `s-park` also passes over a search whose
           candidates never ran. */
        { "s-park-atsink", st_lpark >= S_ARRIVED, "location.hash", SESS_EXPLORE },
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
static int fixture_want_park(void) {
    ColdPreview would;

    cold_park_preview(&would);
    return would.deepcands > 0;
}

int main(int argc, char **argv) {
    JSRuntime *rt;
    trusted_types_selftest();
    policy_container_selftest();
    csp_url_matching_selftest();
    document_policy_selftest();
    rt = JS_NewRuntime();
    JS_SetMaxStackSize(rt, 4 * 1024 * 1024);   /* align quickjs's overflow check with the emcc 8MB wasm stack */
    JSContext *ctx = JS_NewContext(rt);

    concolic_init(ctx);
    flow_registry_init("fixture");   /* one document in this fixture; the world namespace is named by it */
    world_registry_selftest(ctx);   /* the peer half: worlds minted as if by another document */
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
        JSValue root_proxy = window_proxy_new_self(ctx, world_local_doc(), "");
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
    engine_run(ctx, scripts.bodies, scripts.srcs, scripts.types, scripts.n, cold_residue);   /* @H + @S detection */
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
        printf("@COLDPARK {\"records\":%ld,\"segs\":%ld,\"flows\":%ld,\"cands\":%ld,"
               "\"bytes\":%zu,\"store\":\"%s\"}\n",
               cold_park_records(), g_cp.segs, g_cp.flows, g_cp.cands, strlen(recipes),
               cold_park_path ? cold_park_path : "-");
    }
    if (cold_resume_path) {
        cold_resumed(&g_cr);
        printf("@COLDRESUME {\"segs\":%ld,\"flows\":%ld,\"cands\":%ld}\n",
               g_cr.segs, g_cr.flows, g_cr.cands);
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
