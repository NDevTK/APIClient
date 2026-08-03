/* End-to-end proof of the rebuilt frame-agnostic forced-execution core: a single concolic branch must explore
 * BOTH arms via the dispatch loop, with the fork expressed purely as a decision-vector sibling (no OP_if
 * rewind, no frame snapshot). Runs standalone against clean upstream quickjs + the new solver components. */
#include "quickjs.h"
#include "check.h"
#include "solver/concolic.h"
#include "solver/decide.h"
#include "solver/flow.h"
#include "solver/engine.h"
#include "solver/cow.h"
#include "core/loader/document_scripts.h"
#include "core/frame/navigator.h"
#include "core/frame/screen.h"
#include "core/dom/abort.h"
#include "core/dom/node.h"
#include "core/dom/element.h"
#include "core/dom/document.h"
#include "core/events/event_target.h"
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
static JSValue js_append_child(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
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

/* the SAFE-GET fetch host-edge that RETURNS ITS RESULT: models `await fetch('/api/config').then(r=>r.json())`.
   A real network GET is ASYNCHRONOUS, so it returns a PENDING promise + registers its resolve capability and the
   JSON body it will deliver. A flow that awaits it PARKS (its async body suspends on the promise); when the
   frontier stalls, engine_run resolves every pending fetch (the network completing) and the awaiting continuation
   resumes with the body — so body.region flows into a later endpoint as a CONCRETE example. This is the real
   fetch-await path (no bespoke JS global — the page bundle's own `await fetch(...)` drives it). */
static JSValue js_fetch_json(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc > 0) endpoint_record(ctx, "GET", argv[0]);
    JSValue body = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, body, "region", JS_NewString(ctx, "us-west-2"));   /* the fetched config's field */
    JSValue resolving[2];
    JSValue promise = JS_NewPromiseCapability(ctx, resolving);
    engine_pending_fetch(ctx, resolving[0], body);   /* live GET: resolved when the frontier stalls (fetch-await park) */
    JS_FreeValue(ctx, resolving[0]); JS_FreeValue(ctx, resolving[1]);
    JS_FreeValue(ctx, body);
    return promise;   /* PENDING — awaiting it parks the flow until the scheduler drains the fetch */
}

/* the fetch host-edge: funnel into the real @H endpoint surface (dedup + shape happen there). */
static JSValue js_fetch(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc > 0) {
        const char *method = "GET", *mc = NULL;
        if (argc > 1 && JS_IsObject(argv[1])) {
            JSValue m = JS_GetPropertyStr(ctx, argv[1], "method");
            if (JS_IsString(m)) { mc = JS_ToCString(ctx, m); if (mc) method = mc; }
            JS_FreeValue(ctx, m);
        }
        endpoint_record(ctx, method, argv[0]);
        if (mc) JS_FreeCString(ctx, mc);
    }
    return JS_UNDEFINED;
}

/* the "page": a real 2-<script> HTML document. Script 1 reads injected `state` into a config; script 2 (sharing
   globals) branches on it + does a baseline mutation (globalThis.n) first. Exercises: real Lexbor boot,
   cross-script concolic flow (fork on cfg.admin set by script 1), the moat (gated /api/admin), AND per-flow COW
   (both arms see n==1). */
static const char *HTML =
    "<!doctype html><html><body>"
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
    "var ac = new AbortController(); ac.signal.addEventListener('abort', function(){ fetch('/api/aborted?v=fired'); }); ac.abort();"   /* the controller's signal is the REAL state machine: abort() reads [[Signal]] as an internal slot and fires `abort`, whose listener runs as its own task on this flow */
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
    "(async function(){ var c = await fetchJson('/api/config'); fetch('/api/user?region=' + c.region); })();"   /* FETCH-AWAIT-RESULT: await a safe GET, its JSON body's field flows into a later endpoint as a concrete example */
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
    "</script>"
    "</body></html>";

int main(void) {
    JSRuntime *rt = JS_NewRuntime();
    JS_SetMaxStackSize(rt, 4 * 1024 * 1024);   /* align quickjs's overflow check with the emcc 8MB wasm stack */
    JSContext *ctx = JS_NewContext(rt);

    concolic_init(ctx);
    flow_registry_init();
    endpoint_init();
    solve_init(ctx);

    /* BASELINE setup (mark 0): the globals here must NOT be captured, so install the COW hook AFTER. */
    JSValue g = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, g, "fetch", JS_NewCFunction(ctx, js_fetch, "fetch", 1));
    JS_SetPropertyStr(ctx, g, "loadScript", JS_NewCFunction(ctx, js_load_script, "loadScript", 1));   /* lazy-chunk load */
    JS_SetPropertyStr(ctx, g, "eval", JS_NewCFunction(ctx, js_eval_sink, "eval", 1));   /* the eval sink */
    JS_SetPropertyStr(ctx, g, "setInnerHTML", JS_NewCFunction(ctx, js_html_sink, "setInnerHTML", 1));   /* the innerHTML sink */
    JS_SetPropertyStr(ctx, g, "setLocation", JS_NewCFunction(ctx, js_url_sink, "setLocation", 1));   /* the location/URL sink */
    JS_SetPropertyStr(ctx, g, "setBodyAttr", JS_NewCFunction(ctx, js_set_body_attr, "setBodyAttr", 2));   /* DOM attr write (per-flow) */
    JS_SetPropertyStr(ctx, g, "getBodyAttr", JS_NewCFunction(ctx, js_get_body_attr, "getBodyAttr", 1));   /* DOM attr read (per-flow) */
    JS_SetPropertyStr(ctx, g, "fetchJson", JS_NewCFunction(ctx, js_fetch_json, "fetchJson", 1));   /* safe GET -> awaited JSON body */
    JS_SetPropertyStr(ctx, g, "appendChild", JS_NewCFunction(ctx, js_append_child, "appendChild", 1));   /* DOM node insert (per-flow) */
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
    event_target_init(ctx);
    event_target_install(ctx, g);
    abort_init(ctx);
    abort_install(ctx, g);

    /* Browser layer: parse the document with the real Lexbor HTML parser BEFORE the DOM interfaces install,
       because `document` is a wrapper over this tree — the parse itself creates no JS object, so it belongs on
       the baseline beside the globals rather than after the hooks. */
    int asan_min = getenv("APICLIENT_ASAN_MIN") != NULL;   /* fast per-change memory gate: the minimal clone/COW doc */
    const char *doc = asan_min ? HTML_MIN : HTML;
    lxb_html_document_t *dom = lxb_html_document_create();
    lxb_html_document_parse(dom, (const lxb_char_t *)doc, strlen(doc));
    g_body = lxb_dom_interface_element(lxb_html_document_body_element(dom));   /* the DOM sink's target element */

    /* THE REAL DOM, so the tree components are exercised by a fixture at all. They had none: the page above
       reached the tree through host-edge stand-ins (setBodyAttr, appendChild), so node.c, element.c and
       document.c — every wrapper, every prototype, every IDL coercion in them — ran only in the shipped ABI
       build where nothing asserts on the result. */
    element_init(ctx);
    document_install(ctx, g, dom, "https://x.test");
    JS_FreeValue(ctx, g);

    /* The two hook SETS the solver owns, each declared by its own component. They were struct literals here
       and again in test_forced.c, and the pair had drifted. */
    cow_install_time_travel_hooks();
    concolic_install_hooks();

    DocScripts scripts = document_exec_scripts(dom);   /* each <script> its own program body — no concat */
    engine_run(ctx, scripts.bodies, scripts.srcs, scripts.n);         /* @H + @S detection */
    /* No verify call: the candidate re-fires are FLOWS on the same frontier, so engine_run already ran them. */
    doc_scripts_free(&scripts);
    lxb_html_document_destroy(dom);

    /* ONE result document — both surfaces and the scheduler's interleave count, serialized DIRECTLY from the
       C findings (no JS-object round-trip). The host does one JSON.parse of this line and relays it; it used
       to be two lines here, which meant whoever consumed them assembled the document, and assembling is
       structure. The assertions below read the same string, so they cover the composed shape. */
    char *js = result_json();
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
    /* FETCH-AWAIT-RESULT: `await fetchJson('/api/config')` delivered the JSON body, whose .region flowed into
       /api/user?region=us-west-2 as a CONCRETE example — a safe GET's result driving API-value learning. */
    int fetch_await = (strstr(js, "\"/api/user\"") && strstr(js, "us-west-2"));
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
    int domidl_tt   = domproto_tt && cdnull_tt && tcnull_tt && nodeval_tt && tcset_tt;
    int deadline_tt = (strstr(js, "\"/api/deadline\"") && strstr(js, "expired") && strstr(js, "live"));

    int h_ok = asan_min
        ? (fefork_tt && owfork_tt && genfork_tt && gen2fork_tt && genofork_tt && afromfork_tt && spreadfork_tt && setaddfork_tt && setfork_tt && mapmutfork_tt && afsfork_tt && paffork_tt && paf2fork_tt && redfork_tt && rerepfork_tt && gcallfork_tt && gapplyfork_tt && grefapplyfork_tt)   /* MIN gate: just the clone/COW/generator paths */
        : (has_uid_param && role_admin && role_public && merged && pinned && lazy && uafork_tt && touchfork_tt && layoutfork_tt && abortfire_tt && deadline_tt && idlcoerce_tt && domidl_tt && dom_tt && accessor_tt && async_tt && await_tt && asynccall_tt && async_throw && async_preempt && fetch_await && pending_await && promise_state && delete_iso && floc_iso && fefork_tt && pushfork_tt && mapfork_tt && owfork_tt && genfork_tt && gen2fork_tt && genofork_tt && afromfork_tt && spreadfork_tt && setaddfork_tt && setfork_tt && mapmutfork_tt && afsfork_tt && paffork_tt && paf2fork_tt && redfork_tt && rerepfork_tt && gcallfork_tt && gapplyfork_tt && grefapplyfork_tt);
    printf("@H %s (pinned=%d lazy=%d dom-attr=%d dom-node=%d accessor=%d async=%d await=%d asynccall=%d throw=%d preempt=%d fetch=%d pending=%d promise-state=%d delete-iso=%d floc-iso=%d fefork=%d pushfork=%d mapfork=%d owfork=%d genfork=%d gen2fork=%d genofork=%d afromfork=%d spreadfork=%d setaddfork=%d setfork=%d mapmutfork=%d afsfork=%d paffork=%d paf2fork=%d redfork=%d rerepfork=%d gcallfork=%d gapplyfork=%d grefapplyfork=%d ua=%d touch=%d layout=%d abort=%d deadline=%d idl=%d dom-idl=%d)\n",
           h_ok ? "OK" : "FAIL", pinned, lazy, dom_attr, dom_node, accessor_tt, async_tt, await_tt, asynccall_tt, async_throw, async_preempt, fetch_await, pending_await, promise_state, delete_iso, floc_iso, fefork_tt, pushfork_tt, mapfork_tt, owfork_tt, genfork_tt, gen2fork_tt, genofork_tt, afromfork_tt, spreadfork_tt, setaddfork_tt, setfork_tt, mapmutfork_tt, afsfork_tt, paffork_tt, paf2fork_tt, redfork_tt, rerepfork_tt, gcallfork_tt, gapplyfork_tt, grefapplyfork_tt, uafork_tt, touchfork_tt, layoutfork_tt, abortfire_tt, deadline_tt, idlcoerce_tt, domidl_tt);

    /* @S: the eval sink reached by concolic state.code, breakout constructed + fire-verified. Read from the
       ONE document above — there is no second line to keep in step with it. */
    const char *ss = js;
    /* @S JS: single-quote-context breakout fire-verified. @S HTML: innerHTML sink fire-verified via Lexbor re-parse. */
    int s_eval = strstr(ss, "\"sink\":\"eval\"") && strstr(ss, "{state}.code") && strstr(ss, "';X9();//");
    int s_html = strstr(ss, "\"sink\":\"innerHTML\"") && strstr(ss, "{state}.html") && strstr(ss, "<svg onload=X9()>");
    int s_url = strstr(ss, "\"sink\":\"location\"") && strstr(ss, "{state}.next") && strstr(ss, "javascript:X9()");
    int s_ok = s_eval && s_html && s_url;

    printf("%s\n", (h_ok && s_ok)
        ? "PASS: @H merge AND @S eval + innerHTML + location — 3 sink contexts, all SEARCHED + fire-verified"
        : "FAIL: @H or @S incorrect");

    free(js);
    solve_free();
    endpoint_free();

    abort_free(ctx);
    document_free(ctx);   /* the window reference the lifecycle holds */
    element_free(ctx);    /* the wrapper identity table and the DOM interface prototypes */
    event_target_free(ctx);
    flow_registry_free(ctx);
    JS_RunGC(rt);   /* collect flow-local garbage from the runs before teardown */
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    return (h_ok && s_ok) ? 0 : 1;
}
