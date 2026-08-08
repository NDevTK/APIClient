/* End-to-end proof of the rebuilt frame-agnostic forced-execution core: a single concolic branch must explore
 * BOTH arms via the dispatch loop, with the fork expressed purely as a decision-vector sibling (no OP_if
 * rewind, no frame snapshot). Runs standalone against clean upstream quickjs + the new solver components. */
#include "quickjs.h"
#include "check.h"
#include "solver/concolic.h"
#include "solver/decide.h"
#include "solver/flow.h"
#include "solver/world.h"
#include "core/frame/navigable.h"
#include "core/timing/timer.h"
#include "core/frame/window.h"
#include "core/frame/window_proxy.h"
#include "core/frame/remote_object.h"
#include "core/html/html_iframe.h"
#include "solver/engine.h"
#include "solver/cow.h"
#include "core/loader/document_scripts.h"
#include "core/frame/navigator.h"
#include "core/frame/screen.h"
#include "core/dom/abort.h"
#include "core/html/unhandled_rejection.h"
#include "core/dom/node.h"
#include "core/dom/element.h"
#include "core/frame/location.h"
#include "core/idl_args.h"
#include "core/dom/document.h"
#include "core/events/event.h"
#include "core/events/message_event.h"
#include "core/events/message_port.h"
#include "core/frame/policy_container.h"
#include "core/events/event_target.h"
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
#include "solver/endpoint.h"
#include "solver/result.h"
#include "solver/solve.h"
#include "solver/dom_cow.h"   /* dom_attr_capture — the DOM write host-edge records into the per-flow DOM delta */
#include <lexbor/html/html.h>
#include <lexbor/dom/dom.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* the eval host-edge: a code-execution sink — funnel into the @S solver. */
static JSValue js_eval_sink(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc > 0) solve_eval_sink(ctx, argv[0]);
    return JS_UNDEFINED;
}
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
            if (strstr(url, "admin")) engine_queue_script("fetch('/api/admin/audit-log');");   /* chunk-only endpoint */
            /* A chunk that THROWS is what a page error IS — the report must name the capability. A DOMException
               is the most common throw in a DOM engine and keeps its name and message behind prototype
               accessors, so an own-property reader calls it "an object with no own name/message". */
            if (strstr(url, "cethrow")) engine_queue_script("customElements.define('nohyphen', class {});");
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
    if (name && val) dom_cow_set_attribute(g_body, name, val, strlen(val));
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

/* THE HOST OWES SOMETHING while a fetch reply is outstanding OR a flow is blocked on a synchronous request.
   Both are "the trusted zone has work to do"; only the second stops a flow dead, which is why the engine keeps
   them on one register and the fixture answers both here. */
static int fixture_owed(void) { return engine_pending_urls()[0] != 0 || engine_host_requests()[0] != 0; }

static int fixture_provide(JSContext *ctx) {
    const char *urls = engine_pending_urls();
    JSValue body = JS_NewString(ctx, "{\"region\":\"us-west-2\"}");
    int filled = 0;
    while (*urls) {
        const char *nl = strchr(urls, '\n');
        size_t len = nl ? (size_t)(nl - urls) : strlen(urls);
        char *one = malloc(len + 1);
        CHECK(one, "the fixture could not name the URL it is answering");
        memcpy(one, urls, len); one[len] = 0;
        filled += engine_provide(ctx, one, body);
        free(one);
        if (!nl) break;
        urls = nl + 1;
    }
    JS_FreeValue(ctx, body);
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
    "fetch('/api/baseuri?v=' + (document.body.baseURI === 'https://x.test' ? 'base' : 'wrong'));"
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
         - a JS-context sink fed from location.hash IS a real XSS (`';X9();//` arrives intact), and
         - an HTML-context sink fed from the same source is NOT (the `<` arrives as %3C and parses as text).
       Before the delivery transform existed the solver handed the payload over raw and would report BOTH as
       working, which is a false PoC — the thing this half of the engine must never produce.
       BOTH HALVES ARE ASSERTED, through ONE source into TWO sink contexts, which is the only arrangement that
       can tell "the transform is applied" apart from "the solver is failing to solve": the apostrophe survives
       the fragment set so the JS sink FIRES (through `'#';X9();//'` — the leading `#` included), while `<` does
       not survive so the HTML sink CANNOT, and it is reported as a PARKED SEARCH rather than omitted. The
       negative half costs the run five extra candidate re-fires that are known not to fire; that cost buys the
       one property the @S half must never lose, so it is paid on every build. */
    "eval(\"'\" + location.hash + \"'\");"
    "var lhHost = document.createElement('div'); document.body.appendChild(lhHost);"
    "lhHost.innerHTML = location.hash;"
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
    "eval(\"'\" + state.code + \"'\");"   /* @S JS: source lands INSIDE a single-quoted string -> breakout ';X9();// */
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
    /* §5 Headers. The RECORD fill is the conversion `fetch(u, {headers: {...}})` performs, so it is exercised
       through the interface that states it: a record init, then the members that read it back. The list keeps
       PAIRS — two `set-cookie` appends stay two entries and getSetCookie reads both — while `get` combines them
       per §2.2.4, which is the whole difference between a header list and a map. A name is lowercased on the
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
    /* THE TRANSPORT REQUIREMENT REACHES THE SURFACE. `init.headers` is read and converted, and the endpoint
       carries what the request needs — which is the half of "usable" the @H surface never had. The
       Authorization value is built out of `state`, so it is a CONCOLIC and reports its SHAPE: the `{hole}` is
       what tells a reviewer this one is a runtime value to supply, while the two literal headers are the real
       strings the code computed. Inventing a token for the first would be a fabricated observation. */
    "fetch('/api/needsauth', { method: 'POST', headers: {"
      " 'Authorization': 'Bearer ' + state.token,"
      " 'X-Api-Version': '2024-11-01',"
      " 'Content-Type': 'application/json' } });"
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

/* HTML §7.2.6 AND CSP §6.1, in C — the browser half's tests are C tests, and this one has no page to run.
   What it pins is the pair of facts the rest of the platform will build on: what a policy PERMITS, and that a
   child's container is a CLONE whose answers do not move when the parent's would. */
static void policy_container_selftest(void)
{
    PolicyContainer *none, *self_only, *inline_ok, *nonced, *evals, *child;

    none = policy_container_new(NULL, NULL);
    /* No policy is not an empty policy: a document with no Content-Security-Policy permits everything, which
       is the overwhelmingly common case and the one a wrong default would mis-report on every page. */
    CHECK(policy_allows(none, POLICY_INLINE_HANDLER), "no policy must permit an inline handler");
    CHECK(policy_allows(none, POLICY_EVAL), "no policy must permit eval");

    self_only = policy_container_new("script-src 'self'", NULL);
    /* §S's own example: an inline onerror is DEAD under `script-src 'self'`, and so is a javascript: URL. A
       host source never permits inline execution — that is what 'unsafe-inline' is for. */
    CHECK(!policy_allows(self_only, POLICY_INLINE_HANDLER), "'self' must not permit an inline handler");
    CHECK(!policy_allows(self_only, POLICY_JAVASCRIPT_URL), "'self' must not permit a javascript: URL");
    CHECK(!policy_allows(self_only, POLICY_EVAL), "'self' must not permit eval");

    inline_ok = policy_container_new("default-src 'none'; script-src 'unsafe-inline'", NULL);
    CHECK(policy_allows(inline_ok, POLICY_INLINE_HANDLER), "'unsafe-inline' must permit an inline handler");
    CHECK(!policy_allows(inline_ok, POLICY_EVAL), "'unsafe-inline' must not permit eval");

    /* CSP §6.1: a nonce source makes 'unsafe-inline' be IGNORED — the rule that makes adding a nonce to a
       legacy policy actually tighten it rather than widen it. A handler can carry no nonce, so it stays dead. */
    nonced = policy_container_new("script-src 'unsafe-inline' 'nonce-abc'", NULL);
    CHECK(!policy_allows(nonced, POLICY_INLINE_SCRIPT), "a nonce source must make 'unsafe-inline' ignored");
    CHECK(!policy_allows(nonced, POLICY_INLINE_HANDLER), "a handler carries no nonce, so it stays blocked");

    /* CSP §6.1: a present `script-src` REPLACES `default-src` for scripts rather than adding to it. The first
       version of this parser OR'd every script-governing directive's sources into one flag set, and this is
       the case that exposes it — the handler came out ALLOWED because `default-src`'s 'unsafe-inline' survived
       a `script-src` that does not carry it. */
    {
        PolicyContainer *overridden = policy_container_new("default-src 'unsafe-inline'; script-src 'self'", NULL);
        CHECK(!policy_allows(overridden, POLICY_INLINE_HANDLER),
              "a present script-src must REPLACE default-src for scripts, not inherit its 'unsafe-inline'");
        policy_container_free(overridden);
    }
    /* §6.1's granular forms, and the fact that they are chosen per KIND: script-src-attr governs handlers and
       javascript: URLs, script-src-elem governs script elements, and eval falls past both to script-src. */
    {
        PolicyContainer *granular =
            policy_container_new("script-src 'unsafe-inline' 'unsafe-eval'; script-src-attr 'none'", NULL);
        CHECK(!policy_allows(granular, POLICY_INLINE_HANDLER), "script-src-attr 'none' must kill a handler");
        CHECK(!policy_allows(granular, POLICY_JAVASCRIPT_URL), "script-src-attr governs javascript: URLs too");
        CHECK(policy_allows(granular, POLICY_INLINE_SCRIPT),
              "script-src-attr must not govern a script ELEMENT — that is script-src-elem's fallback to script-src");
        CHECK(policy_allows(granular, POLICY_EVAL), "eval has no granular form and reads script-src");
        policy_container_free(granular);
    }
    /* §2.2.1: within ONE policy a repeated directive is IGNORED, so the first wins... */
    {
        PolicyContainer *dup = policy_container_new("script-src 'unsafe-inline'; script-src 'self'", NULL);
        CHECK(policy_allows(dup, POLICY_INLINE_HANDLER), "a repeated directive in one policy must be ignored");
        policy_container_free(dup);
    }
    /* ...but §2.2's policy LIST is comma-delimited and enforced INDEPENDENTLY, so the very same two directives
       as two POLICIES must intersect instead. These two lines differ by one character and must not agree. */
    {
        PolicyContainer *list = policy_container_new("script-src 'unsafe-inline', script-src 'self'", NULL);
        CHECK(!policy_allows(list, POLICY_INLINE_HANDLER),
              "policies in a list are enforced independently — a second policy can only NARROW");
        policy_container_free(list);
    }
    /* A policy that governs no script directive forbids nothing about scripts. */
    {
        PolicyContainer *unrelated = policy_container_new("img-src 'none'; frame-ancestors 'none'", NULL);
        CHECK(policy_allows(unrelated, POLICY_INLINE_HANDLER), "img-src must not block a handler");
        CHECK(policy_allows(unrelated, POLICY_EVAL), "frame-ancestors must not block eval");
        policy_container_free(unrelated);
    }

    evals = policy_container_new("script-src 'unsafe-eval'", NULL);
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

/* THE SCAN THAT MAKES THE CONTAINER LOAD-BEARING, which the parser test above does not reach: the parse can be
   perfect and still answer for nothing if the document's own `<meta>` never gets to it. Its own tree, because
   that is what makes it exercisable at all. */
static void meta_policy_selftest(void)
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
    lxb_html_document_t *dom = lxb_html_document_create();
    PolicyContainer *p, *empty;
    lxb_html_document_t *plain;

    lxb_html_document_parse(dom, (const lxb_char_t *)SRC, strlen(SRC));
    p = document_meta_policy(dom);
    CHECK(policy_container_csp(p) != NULL, "the meta scan found no policy in a document that declares two");
    CHECK(!policy_allows(p, POLICY_INLINE_HANDLER),
          "two meta policies must INTERSECT — the second's 'self' forbids what the first's 'unsafe-inline' "
          "permits, and a scan that let the last one win would report a live handler on a page that blocks it");
    CHECK(!policy_allows(p, POLICY_EVAL), "neither meta policy carries 'unsafe-eval'");

    /* A page with no CSP at all is the overwhelmingly common one, and the scan must not invent a policy for
       it — an empty container that answered "blocked" would suppress every real finding on every such page. */
    plain = lxb_html_document_create();
    lxb_html_document_parse(plain, (const lxb_char_t *)"<html><body></body></html>", 26);
    empty = document_meta_policy(plain);
    CHECK(policy_allows(empty, POLICY_INLINE_HANDLER), "a document with no meta CSP must permit everything");

    policy_container_free(p);
    policy_container_free(empty);
    lxb_html_document_destroy(dom);
    lxb_html_document_destroy(plain);
}

/* THE CROSS-INSTANCE HALF OF THE COW DELTA. One WASM instance is one document regardless of origin, so a flow
   that scripts an iframe or a popup owns segments in two instances; the delta cannot travel (it names its
   targets by live heap pointers) so the world's NAME travels and the peer builds its own segment.
   This exercises the peer's half against worlds minted as if by another document — the sender's half is
   exercised by every fork the fixture below performs, since flow_add mints and engine_fork_finalize forks. */
static void world_registry_selftest(JSContext *ctx)
{
    /* Worlds minted "elsewhere": doc 7, a chain root -> parent -> child. This instance is doc 1. */
    WorldId root = { 7, 1 }, parent = { 7, 2 }, child = { 7, 3 }, stranger = { 7, 99 };
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
    *presult = engine_host_take(ctx, s->req);
    s->req = 0;
    return JS_STEP_DONE;
}

static const IdlStepDecl HOSTREQ_DECL = { hostreq_step, sizeof(HostReqState), hostreq_visit, NULL };

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
            } else {
                v = JS_NewStringLen(ctx, tab + 1, (size_t)(end - tab - 1));
            }
            /* ONE ANSWER PER REQUEST. Answering inside a branch AND here answered twice, which the engine's own
               assert named at the site — a second answer would overwrite a value the asking machine may
               already have read. */
            n += engine_host_answer(ctx, id, v);
            JS_FreeValue(ctx, v);
        }
        p = end + 1;
    }
    return n;
}

/* THE AGENT AND THE DOCUMENT, SPLIT — the same split wpt_runner.c and main.c carry, for the same reason: a
   SAME-ORIGIN CHILD NAVIGABLE IS A SECOND DOCUMENT IN THIS AGENT (HTML's similar-origin window agent is one
   heap), so what a document of this build IS has to be one description that runs twice. This fixture's probes
   read `_cw.parent === window` through exactly that child. */
static int g_id_host_read, g_id_append_child;   /* declared once per agent — a member has one pool entry */

static void tf_agent_init(JSContext *ctx)
{
    static const IdlArgType HR_ARGS[1] = { IDL_DOMSTRING };
    static const IdlArgType ONE_STR[1] = { IDL_DOMSTRING };

    /* THE SYNCHRONOUS HOST READ. A DECLARED step member, because suspending and answering at the same call
       site is the only thing a plain C body cannot do. */
    g_id_host_read = idl_method_id_step(ctx, HR_ARGS, 1, NULL, 0, &HOSTREQ_DECL, 0);
    /* A DECLARED member, like every DOM member — this host-edge mutates the tree, and §4.2.3's insertion steps
       are drained by the machine every declared member converges on. */
    g_id_append_child = idl_method_id(ctx, ONE_STR, 1, js_append_child, 0);
    { static const FetchProvider P = { engine_pending_fetch_url }; fetch_set_provider(&P); }
    timer_set_script_sink(engine_queue_script);   /* §8.6: a STRING handler is evaluated, as a flow */
    event_target_init(ctx);
    window_init(ctx);
    location_init(ctx);
    navigable_init(ctx);
    timer_init(ctx);
    window_proxy_init(ctx, "https://x.test");
    remote_object_init(ctx);   /* §7.2.5.1's object half */
    window_proxy_install_members(ctx);   /* §7.2.5.1: local reads answer now, remote ones SUSPEND */
    event_init(ctx);
    /* HTML §8.1.7.5: a rejection nobody handles is a page error, and it was invisible. */
    unhandled_rejection_init(ctx);
    abort_init(ctx);
    element_init(ctx);
    iframe_init(ctx);
    document_init(ctx);   /* §4.8.5: the slot a child navigable lives in */
}

/* ONE DOCUMENT — run once per document including the first. */
static void tf_realm_install(JSContext *ctx, lxb_html_document_t *dom, const char *url, const char *origin,
                             uint32_t doc_id)
{
    JSValue g = JS_GetGlobalObject(ctx);
    /* THE HOST'S NETWORK. SECURITY.md puts every byte of it behind the trusted chokepoint, so this host's
       answer is to PARK the request on the flow's pending register and let the trusted zone fetch it. */
    fetch_install(ctx, g);   /* the REAL component: `fetch`, and with it Response and Headers */
    JS_SetPropertyStr(ctx, g, "loadScript", JS_NewCFunction(ctx, js_load_script, "loadScript", 1));   /* lazy-chunk load */
    JS_SetPropertyStr(ctx, g, "eval", JS_NewCFunction(ctx, js_eval_sink, "eval", 1));   /* the eval sink */
    JS_SetPropertyStr(ctx, g, "setInnerHTML", JS_NewCFunction(ctx, js_html_sink, "setInnerHTML", 1));   /* the innerHTML sink */
    JS_SetPropertyStr(ctx, g, "setLocation", JS_NewCFunction(ctx, js_url_sink, "setLocation", 1));   /* the location/URL sink */
    /* THE REAL Location, which the smoke test had never exercised: `setLocation` above stands in for the URL
       SINK, and the two attacker SOURCES behind `location.hash`/`location.search` were reached by nothing. That
       left the per-component percent-encode sets — the thing that decides whether an @S PoC reproduces in a
       browser at all — with no test of any kind. */
    location_install(ctx, g, url);
    /* HTML §8.6's TIMER TASK SOURCE. The fixture had none, so `setTimeout` was simply absent and any probe
       using one threw — which is how a great deal of real page code reaches the event loop, and it was the one
       platform edge the engine's own test could not exercise. */
    timer_install(ctx, g);
    /* §7.2.2's BROWSING-CONTEXT SURFACE, which this fixture did not have at all: `window`, `self`, `parent`,
       `top`, `closed`, `close()`, the bars. A probe that reads `_cw.parent === window` cannot be written
       without it, and a document with no `window` is not a document any page script would survive. */
    /* §2.7 BEFORE §7.2.5: Window.prototype is CHAINED to EventTarget.prototype, so the prototype has to
       exist before the window is installed. */
    window_install(ctx, g, url);
    /* §7.4's `window.open`, which hands back a WindowProxy for a document in ANOTHER instance at its own call
       site — the child's name is minted in this instance, so nothing suspends. */
    navigable_install(ctx, g, origin);
    /* THE SYNCHRONOUS HOST READ. A DECLARED step member, because suspending and answering at the same call
       site is the only thing a plain C body cannot do. */
    idl_install_method(ctx, g, "hostRead", 1, g_id_host_read);
    JS_SetPropertyStr(ctx, g, "setBodyAttr", JS_NewCFunction(ctx, js_set_body_attr, "setBodyAttr", 2));   /* DOM attr write (per-flow) */
    JS_SetPropertyStr(ctx, g, "getBodyAttr", JS_NewCFunction(ctx, js_get_body_attr, "getBodyAttr", 1));   /* DOM attr read (per-flow) */
    {
        /* A DECLARED member, like every DOM member — this host-edge mutates the tree, and §4.2.3's insertion
           steps are drained by the machine every declared member converges on. As a raw JS_CFUNC_DEF its steps
           never ran at all; nothing showed it, because the <span> it appends is neither a script nor a custom
           element. The engine asserts on exactly this now, which is what caught it. */
        idl_install_method(ctx, g, "appendChild", 1, g_id_append_child);
    }
    JS_SetPropertyStr(ctx, g, "lastChildMark", JS_NewCFunction(ctx, js_last_child_mark, "lastChildMark", 0));   /* DOM node read */
    JS_SetPropertyStr(ctx, g, "state", concolic_new(ctx, "{state}", "{state}", JS_UNDEFINED));   /* injected/unknown app state */
    /* the REAL component, not a synthetic edge: a UA/touch gate is where a bundle hides its other endpoints,
       so this fixture exercises the interface the ABI build installs rather than a stand-in for it. */
    navigator_install(ctx, g);
    screen_install(ctx, g);
    /* The components the ABI entry installs, so this fixture runs the engine that ships. Unblocked by the
       JS_AddIntrinsicDOMException fix: the intrinsic is per-context idempotent now, so JS_NewContext's own
       JS_AddIntrinsicAToB install plus this explicit one no longer overwrite one prototype with another. */
    CHECK(JS_AddIntrinsicDOMException(ctx) == 0, "the DOMException intrinsic failed to install");
    event_install(ctx, g);   /* the Event interface object — `new Event(...)` and every `instanceof Event` */
    /* §2.7: the global reaches add/removeEventListener/dispatchEvent through Window.prototype ->
       EventTarget.prototype, which window_install chains it to. */
    event_target_set_window(ctx, g);   /* §7.6: the document's parent on a propagation path */
    /* HTML §8.1.7.2: window's IDL mixes in GlobalEventHandlers AND WindowEventHandlers — `window.onload` is
       how a great deal of real code starts. */
    event_target_install_handlers(ctx, g, EH_GLOBAL | EH_WINDOW);
    unhandled_rejection_install(ctx, g);   /* PromiseRejectionEvent */
    abort_install(ctx, g);

    /* Browser layer: parse the document with the real Lexbor HTML parser BEFORE the DOM interfaces install,
       because `document` is a wrapper over this tree — the parse itself creates no JS object, so it belongs on
       the baseline beside the globals rather than after the hooks. */

    /* THE REAL DOM, so the tree components are exercised by a fixture at all. They had none: the page above
       reached the tree through host-edge stand-ins (setBodyAttr, appendChild), so node.c, element.c and
       document.c — every wrapper, every prototype, every IDL coercion in them — ran only in the shipped ABI
       build where nothing asserts on the result. */
    document_install(ctx, g, dom, url, doc_id);
    JS_FreeValue(ctx, g);
}

/* A SAME-ORIGIN CHILD NAVIGABLE'S REALM — a second JSContext in the SAME JSRuntime. */
static JSContext *tf_child_realm(JSRuntime *rt, lxb_html_document_t *dom, const char *url, const char *origin,
                                 uint32_t doc_id)
{
    JSContext *ctx = JS_NewContext(rt);

    CHECK(ctx != NULL, "a same-origin child navigable's realm could not be created");
    world_doc_adopt(doc_id);
    tf_realm_install(ctx, dom, url, origin, doc_id);
    return ctx;
}

int main(void) {
    JSRuntime *rt;
    policy_container_selftest();
    meta_policy_selftest();
    rt = JS_NewRuntime();
    JS_SetMaxStackSize(rt, 4 * 1024 * 1024);   /* align quickjs's overflow check with the emcc 8MB wasm stack */
    JSContext *ctx = JS_NewContext(rt);

    concolic_init(ctx);
    flow_registry_init("fixture");   /* one document in this fixture; the world namespace is named by it */
    world_registry_selftest(ctx);   /* the peer half: worlds minted as if by another document */
    endpoint_init();
    solve_init(ctx);

    /* BASELINE setup (mark 0): the globals here must NOT be captured, so install the COW hook AFTER. */
    tf_agent_init(ctx);
    navigable_set_realm_builder(tf_child_realm);
    int asan_min = getenv("APICLIENT_ASAN_MIN") != NULL;   /* fast per-change memory gate: the minimal clone/COW doc */
    const char *doc = asan_min ? HTML_MIN : HTML;
    lxb_html_document_t *dom = lxb_html_document_create();
    lxb_html_document_parse(dom, (const lxb_char_t *)doc, strlen(doc));
    g_body = lxb_dom_interface_element(lxb_html_document_body_element(dom));   /* the DOM sink's target element */
    tf_realm_install(ctx, dom, "https://x.test/p", "https://x.test", world_local_doc());

    /* The two hook SETS the solver owns, each declared by its own component. They were struct literals here
       and again in test_forced.c, and the pair had drifted. */
    cow_install_time_travel_hooks(engine_gen_fork);
    concolic_install_hooks();
    /* The surface is installed, so every member the platform has is declared — a declaration from here on is a
       per-wrapper or per-flow mint, and that is what the pool asserts against. */
    idl_args_seal();

    /* The trusted host's half: what it is owed, and how it pays. */
    engine_set_stall_hook(fixture_owed);
    engine_set_provider(fixture_provide);

    DocScripts scripts = document_exec_scripts(dom);   /* each <script> its own program body — no concat */
    engine_run(ctx, scripts.bodies, scripts.srcs, scripts.n);         /* @H + @S detection */
    /* No verify call: the candidate re-fires are FLOWS on the same frontier, so engine_run already ran them. */
    doc_scripts_free(&scripts);
    lxb_html_document_destroy(dom);

    /* ONE result document — both surfaces and the scheduler's interleave count, serialized DIRECTLY from the
       C findings (no JS-object round-trip). The host does one JSON.parse of this line and relays it; it used
       to be two lines here, which meant whoever consumed them assembled the document, and assembling is
       structure. The assertions below read the same string, so they cover the composed shape. */
    char *js = result_json(ctx);
    printf("@RESULT %s\n", js);

    int has_uid_param = strstr(js, "\"/api/u\"") && strstr(js, "\"uid\"") && strstr(js, "{state}.id");
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
    int hdrseq = (strstr(js, "\"/api/hdrseq\"") && strstr(js, "g1, g2") && strstr(js, "\"m1\"") &&
                  strstr(js, "\"a1\"") &&
                  strstr(js, "\"bad\",\"validValues\":[\"threw\"]") &&
                  strstr(js, "\"nul\",\"validValues\":[\"threw\"]"));
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
    int nodealgo_tt = !strstr(js, "wrong");
    /* §4.10: submit() derived the GET; the named field carries its SOURCE rather than an invented value; a
       DISABLED control contributed nothing and an UNCHECKED box contributed nothing, both of which only an
       absence can prove; and the second submit AFTER checking it does include it. */
    if (!strstr(js, "\"/api/search\"") || !strstr(js, "{state}.q")) nodealgo_tt = 0;
    if (strstr(js, "\"off\"")) nodealgo_tt = 0;
    if (!strstr(js, "\"agree\"")) nodealgo_tt = 0;
    /* requestSubmit(): the CANCELLED form's action must never appear, and the uncancelled one's must. */
    if (strstr(js, "/api/never")) nodealgo_tt = 0;
    if (!strstr(js, "\"/api/didsubmit\"") || !strstr(js, "\"rs\"")) nodealgo_tt = 0;
    /* §4.13: the async reaction's loop ran to its END across the preempts, not to some suspended partial. */
    if (!strstr(js, "\"44850\"")) nodealgo_tt = 0;
    /* The uncaught DOMException is NAMED in the report, not "an object with no own name/message". */
    if (!strstr(js, "SyntaxError: not a valid custom element name")) nodealgo_tt = 0;
    /* §8.4 outerHTML is the same serialiser over the element ITSELF — its own tag and attributes included. */
    if (!strstr(js, "%3Csection%20data-k%3D%22v%22%3E%3Cp%20class%3D%22q%22%3Ehi%3Cbr%3E%3C%2Fp%3E%3C%2Fsection%3E"))
        nodealgo_tt = 0;
    /* §4.13.3: the old value, the new one, the removal's null — and NOTHING for the unobserved attribute. */
    if (!strstr(js, "\"first\"") || !strstr(js, "\"second\"")) nodealgo_tt = 0;
    if (strstr(js, "data-ignored") || strstr(js, "\"nope\"")) nodealgo_tt = 0;
    /* §8.1.7.5: the rejection nobody handled is reported, and the one handled a microtask later is NOT. */
    if (!strstr(js, "rejNOHANDLER")) nodealgo_tt = 0;
    if (strstr(js, "rejHANDLED")) nodealgo_tt = 0;
    /* …and the one a listener CANCELLED is not reported either — the page answered for it. */
    if (strstr(js, "rejCANCEL")) nodealgo_tt = 0;
    for (unsigned ai = 0; ai < sizeof(NODE_ALGOS) / sizeof(NODE_ALGOS[0]); ai++)
        if (!strstr(js, NODE_ALGOS[ai][0]) || !strstr(js, NODE_ALGOS[ai][1])) nodealgo_tt = 0;
    int domidl_tt   = domproto_tt && cdnull_tt && tcnull_tt && nodeval_tt && tcset_tt;
    int deadline_tt = (strstr(js, "\"/api/deadline\"") && strstr(js, "expired") && strstr(js, "live"));

    /* THE PROBES, DECLARED ONCE. This was a 46-term conjunction and a separate printf listing 43 of them, which
       is two hand-maintained lists of the same thing — so a probe could be computed and joined to NEITHER, and
       one immediately was: `clone_body` asserted §6.4 clone() and the gate never read it, so the fixture
       reported PASS on a result nothing had checked. A row here is what makes a probe a probe: the gate walks
       it and the report walks it, and a probe that is not declared does not exist rather than silently
       passing. `min` marks the subset the ASAN gate runs (the clone/COW/generator paths). */
    struct { const char *name; int ok; unsigned char min; } probes[] = {
        { "uid-param", has_uid_param, 0 }, { "role-admin", role_admin, 0 },
        { "role-public", role_public, 0 }, { "merged", merged, 0 },
        { "pinned", pinned, 0 },           { "lazy", lazy, 0 },
        { "dom-attr", dom_attr, 0 },       { "dom-node", dom_node, 0 },
        { "dom-tt", dom_tt, 0 },           { "accessor", accessor_tt, 0 },
        { "async", async_tt, 0 },          { "await", await_tt, 0 },
        { "asynccall", asynccall_tt, 0 },  { "throw", async_throw, 0 },
        { "preempt", async_preempt, 0 },   { "fetch", fetch_await, 0 },
        { "clone-body", clone_body, 0 },   { "body-bytes", body_bytes, 0 },
        { "body-iso", body_iso, 0 },       { "hdrs", hdrs, 0 },
        { "hdr-proxy", hdrproxy, 0 },      { "needs-auth", needsauth, 0 },
        { "hdr-iter", hdriter, 0 },        { "hdr-seq", hdrseq, 0 },
        { "pending", pending_await, 0 },   { "promise-state", promise_state, 0 },
        { "delete-iso", delete_iso, 0 },   { "floc-iso", floc_iso, 0 },
        { "ua", uafork_tt, 0 },            { "touch", touchfork_tt, 0 },
        { "layout", layoutfork_tt, 0 },    { "abort", abortfire_tt, 0 },
        { "deadline", deadline_tt, 0 },    { "idl", idlcoerce_tt, 0 },
        { "dom-idl", domidl_tt, 0 },       { "node-algo", nodealgo_tt, 0 },
        { "pushfork", pushfork_tt, 0 },    { "mapfork", mapfork_tt, 0 },
        { "fefork", fefork_tt, 1 },        { "owfork", owfork_tt, 1 },
        { "genfork", genfork_tt, 1 },      { "gen2fork", gen2fork_tt, 1 },
        { "genofork", genofork_tt, 1 },    { "afromfork", afromfork_tt, 1 },
        { "spreadfork", spreadfork_tt, 1 },{ "setaddfork", setaddfork_tt, 1 },
        { "setfork", setfork_tt, 1 },      { "mapmutfork", mapmutfork_tt, 1 },
        { "afsfork", afsfork_tt, 1 },      { "paffork", paffork_tt, 1 },
        { "paf2fork", paf2fork_tt, 1 },    { "redfork", redfork_tt, 1 },
        { "rerepfork", rerepfork_tt, 1 },  { "gcallfork", gcallfork_tt, 1 },
        { "gapplyfork", gapplyfork_tt, 1 },{ "grefapplyfork", grefapplyfork_tt, 1 },
        { "hostreq", hostreq_tt, 1 }, { "hostreq-fork", hostreqfork_tt, 1 },
        { "nav-open", navopen_tt, 1 }, { "proxy-sop", sop_tt, 1 }, { "xdoc-read", xdocread_tt, 1 }, { "xdoc-job", xdocjob_tt, 1 }, { "timer-order", timer_tt, 1 }, { "iframe-nav", ifnav_tt, 1 },
    };
    int h_ok = 1;
    printf("@H ");
    for (unsigned pi = 0; pi < sizeof(probes) / sizeof(probes[0]); pi++) {
        if (asan_min && !probes[pi].min)
            continue;
        h_ok = h_ok && probes[pi].ok;
        printf("%s=%d ", probes[pi].name, probes[pi].ok);
    }
    printf("=> %s\n", h_ok ? "OK" : "FAIL");

    /* @S: the eval sink reached by concolic state.code, breakout constructed + fire-verified. Read from the
       ONE document above — there is no second line to keep in step with it. */
    const char *ss = js;
    /* @S JS: single-quote-context breakout fire-verified. @S HTML: innerHTML sink fire-verified via Lexbor re-parse. */
    int s_eval = strstr(ss, "\"sink\":\"eval\"") && strstr(ss, "{state}.code") && strstr(ss, "';X9();//");
    int s_html = strstr(ss, "\"sink\":\"innerHTML\"") && strstr(ss, "{state}.html") && strstr(ss, "<svg onload=X9()>");
    int s_url = strstr(ss, "\"sink\":\"location\"") && strstr(ss, "{state}.next") && strstr(ss, "javascript:X9()");
    /* THE SOURCE'S BROWSER TRANSFORM, asserted end to end: a breakout through the REAL Location, whose value
       the browser percent-encodes per the fragment set and prefixes with `#`. The apostrophe is not in that set,
       so `';X9();//` arrives intact and fires — and it fires through `'#';X9();//'`, the leading `#` included.
       Before the transform existed the payload was handed over raw, so this fired for the wrong reason and an
       HTML-context breakout through the same source would have fired too, which a browser would not. */
    int s_loc = strstr(ss, "\"source\":\"location.hash\"") && strstr(ss, "';X9();//");
    /* THE NEGATIVE HALF, and it is an assertion about the REPORT, not about a missing line. The same source
       into an HTML sink must produce NO PoC — the fragment set encodes `<`, so every HTML candidate arrives as
       `%3C` and parses as text — and must still be REPORTED, as a parked search carrying the encode set that
       defeated it. Asserting only "no PoC" would also pass if the sink were never detected, which is the false
       negative this half exists to catch; asserting the parked entry says the sink WAS reached and searched. */
    int s_park = strstr(ss, "\"sink\":\"innerHTML\",\"source\":\"location.hash\",\"search\":\"parked\"")
              && strstr(ss, "\"sourceEncodes\":\" \\\"<>`\"")
              && !strstr(ss, "\"source\":\"location.hash\",\"poc\":\"<");
    int s_ok = s_eval && s_html && s_url && s_loc && s_park;

    printf("%s\n", (h_ok && s_ok)
        ? "PASS: @H merge AND @S eval + innerHTML + location + a REAL Location source — fired where the source's"
          " transform permits, parked where it does not"
        : "FAIL: @H or @S incorrect");

    free(js);
    solve_free();
    endpoint_free();

    unhandled_rejection_free(ctx);
    abort_free(ctx);
    document_free(ctx);   /* the window reference the lifecycle holds */
    iframe_free(ctx);
    element_free(ctx);    /* the wrapper identity table and the DOM interface prototypes */
    event_target_free(ctx);
    message_port_free(ctx);
    message_event_free(ctx);
    event_free(ctx);
    headers_free(ctx);    /* Headers.prototype and the name it interned */
    url_free(ctx);
    usp_free(ctx);
    transform_stream_free(ctx);
    writable_stream_free(ctx);
    queuing_strategy_free(ctx);
    readable_stream_free(ctx);
    blob_free(ctx);
    location_free();   /* the API base URL the document's address produced */
    encoding_free(ctx);
    text_stream_free(ctx);
    form_data_free(ctx);        /* URLSearchParams.prototype */
    response_free(ctx);
    request_free(ctx);   /* Response.prototype — one object, held for the runtime's life */
    idl_args_free(ctx);   /* the dictionary member atoms the declaration pool interned */
    navigable_free(ctx);
    window_free(ctx);
    remote_object_free(ctx);
    window_proxy_free(ctx);   /* the shared §7.2.5.1 prototype every proxy is chained to */
    flow_registry_free(ctx);
    JS_RunGC(rt);   /* collect flow-local garbage from the runs before teardown */
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    return (h_ok && s_ok) ? 0 : 1;
}
