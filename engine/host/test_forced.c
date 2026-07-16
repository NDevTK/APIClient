/* End-to-end proof of the rebuilt frame-agnostic forced-execution core: a single concolic branch must explore
 * BOTH arms via the dispatch loop, with the fork expressed purely as a decision-vector sibling (no OP_if
 * rewind, no frame snapshot). Runs standalone against clean upstream quickjs + the new solver components. */
#include "quickjs.h"
#include "solver/concolic.h"
#include "solver/decide.h"
#include "solver/flow.h"
#include "solver/engine.h"
#include "solver/cow.h"
#include "core/loader/document_scripts.h"
#include "solver/endpoint.h"
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

/* the SAFE-GET fetch host-edge that RETURNS ITS RESULT: models `await fetch('/api/config').then(r=>r.json())`
   for an idempotent GET the engine decided is safe to actually perform. Records the endpoint, then returns a
   RESOLVED promise wrapping the fetched JSON body (concrete example values). Awaiting it delivers the body to the
   continuation, so a field like body.region flows into a later endpoint as a CONCRETE example — API learning. */
static JSValue js_fetch_json(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc > 0) endpoint_record(ctx, "GET", argv[0]);
    JSValue body = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, body, "region", JS_NewString(ctx, "us-west-2"));   /* the fetched config's field */
    JSValue resolving[2];
    JSValue promise = JS_NewPromiseCapability(ctx, resolving);
    JSValue r = JS_Call(ctx, resolving[0], JS_UNDEFINED, 1, (JSValueConst *)&body);   /* resolve with the body */
    JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, resolving[0]); JS_FreeValue(ctx, resolving[1]);
    JS_FreeValue(ctx, body);
    return promise;
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
    "(async function(){ throw 'asyncThrew'; })().catch(function(e){ fetch('/api/caught?e=' + e); });"   /* async THROW -> rejected promise -> .catch reaction fires */
    "(async function(){ var s=0; for(var i=0;i<2000;i++){ s=s+1; } fetch('/api/asyncloop?s='+s); })();"   /* async body with a LOOP -> preempt may fire inside the async tramp frame */
    "(async function(){ var c = await fetchJson('/api/config'); fetch('/api/user?region=' + c.region); })();"   /* FETCH-AWAIT-RESULT: await a safe GET, its JSON body's field flows into a later endpoint as a concrete example */
    "(async function(){ var p = new Promise(function(res){ Promise.resolve().then(function(){ res('lazyRegion'); }); }); var c = await p; fetch('/api/lazy?r=' + c); })();"   /* PENDING await: the promise resolves LATER via a microtask (stands in for the network) -> park + resume */
    "var _spr; var _sp = new Promise(function(r){ _spr = r; }); _sp.then(function(v){ fetch('/api/shared?v=' + v); }); _spr(state.beta ? 'shBETA' : 'shSTABLE');"   /* PROBE: _sp created BEFORE the state.beta snapshot fork -> shared; settled per-flow. If promise state is NOT per-flow COW, only ONE value survives (contamination) */
    "if (state.gamma) { delete delObj.k; } fetch('/api/tok?t=' + (delObj.k ? delObj.k : 'wasDeleted'));"   /* DELETE time-travel: the gamma-true flow deletes delObj.k; the gamma-false flow must STILL see keepVAL (the delete is per-flow) */
    "var acc = 0; for (var i = 0; i < 15; i++) { acc = acc + i; }"   /* a real LOOP (back-edges) so the quantum fires: a flow yields MID-LOOP and the scheduler interleaves parked sibling flows, exercising the COW+decide+pins swap */
    "function* gen(n){ var s=0; for(var i=0;i<n;i++){ s=s+i; } yield s; yield s*2; }"   /* GENERATOR body with a LOOP: .next() runs the body on the tramp chain, the loop preempts the base flow */
    "var git = gen(2000); fetch('/api/gen?a=' + git.next().value + '&b=' + git.next().value);"
    "function* gg(){ for(var i=0;i<10;i++) yield i*i; } var gsum=0; for(const x of gg()){ gsum = gsum + x; } fetch('/api/genforof?s=' + gsum);"
    "function* gt(){ try { yield 1; } catch(e){ yield 'c:'+e; } } var gi = gt(); gi.next(); fetch('/api/genthrow?v=' + gi.throw('X').value);"   /* direct .next()/.next() -> 1999000, 3998000 */
    "function* gp([a,b,c]){ yield a+b+c; } fetch('/api/genparam?v=' + gp([10,20,30]).next().value);"   /* PARAM-DESTRUCTURING generator: the array destructuring iterates at CREATION on the tramp chain (do_generator_create_tramp) */
    "var _fe=0; [10,20,30].forEach(function(x){ var t=0; for(var i=0;i<50;i++) t++; _fe += x + t; }); fetch('/api/foreach?s=' + _fe);"   /* forEach callback with a LOOP: the callback runs on the tramp chain (do_array_iter_tramp), so its back-edges PARK the base flow and resume — never drive-to-completion -> 60+150=210 */
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
    "let _lex = 7; const _kex = 8; class _Cex { g(){ return _lex + _kex; } } fetch('/api/lexbind?v=' + new _Cex().g());"   /* TOP-LEVEL LEXICAL bindings (let/const/class) define into the SHARED global_var_obj; JS_DefineGlobalVar now COW-captures the creation so it is per-flow, not leaked into snapshot siblings (whose re-definition would else throw redeclaration and kill the sibling + its downstream forks � a silently dropped @H arm) -> 15 */
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
    JS_FreeValue(ctx, g);

    /* per-flow isolation: the time-travel RECORD boundary — capture shared property writes AND shared closure
       CELL writes made DURING flows, so a flow can be rewound/replayed exactly (one structured registration). */
    static const JSTimeTravelHooks TIME_TRAVEL = { .prop_write = cow_capture_hook, .cell_write = cow_capture_varref };
    JS_SetTimeTravelHooks(&TIME_TRAVEL);
    /* concolic VALUE propagation stays installed across BOTH scheduling and verification (taint must flow during
       verify too): `+` builds URL shapes, == / === forks on equality gates + concretizes-on-pin. The exploration
       hooks (branch/fork/preempt) are owned + scoped by the scheduler (engine_run), not installed here. */
    static const JSConcolicHooks CONCOLIC = { .add = concolic_add_hook, .cmp = concolic_cmp_hook };
    JS_SetConcolicHooks(&CONCOLIC);

    /* Browser layer: parse the document with the real Lexbor HTML parser, then extract its executable inline
       scripts as ONE flow program (document_scripts, Blink core/loader). No boot — the scripts ARE the first flow. */
    lxb_html_document_t *dom = lxb_html_document_create();
    lxb_html_document_parse(dom, (const lxb_char_t *)HTML, strlen(HTML));
    g_body = lxb_dom_interface_element(lxb_html_document_body_element(dom));   /* the DOM sink's target element */
    DocScripts scripts = document_exec_scripts(dom);   /* each <script> its own program body — no concat */
    engine_run(ctx, scripts.bodies, scripts.n);         /* @H + @S detection */
    solve_verify(ctx, scripts.bodies, scripts.n);       /* @S: fire-verify the breakout by re-running the real code */
    doc_scripts_free(&scripts);
    lxb_html_document_destroy(dom);

    /* emit the deduped @H surface — serialized DIRECTLY from the C findings (no JS-object round-trip) */
    char *js = endpoint_json();
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
       via decision-vector REPLAY (a frame snapshot of the nested activation is invalid). Both awADMIN and
       awPUBLIC present ⇒ await suspend/resume works AND the nested-activation fork is correct per flow. */
    int await_tt = (strstr(js, "\"/api/await\"") && strstr(js, "awADMIN") && strstr(js, "awPUBLIC"));
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

    int h_ok = (has_uid_param && role_admin && role_public && merged && pinned && lazy && dom_tt && accessor_tt && async_tt && await_tt && async_throw && async_preempt && fetch_await && pending_await && promise_state && delete_iso);
    printf("@H %s (pinned=%d lazy=%d dom-attr=%d dom-node=%d accessor=%d async=%d await=%d throw=%d preempt=%d fetch=%d pending=%d promise-state=%d delete-iso=%d)\n",
           h_ok ? "OK" : "FAIL", pinned, lazy, dom_attr, dom_node, accessor_tt, async_tt, await_tt, async_throw, async_preempt, fetch_await, pending_await, promise_state, delete_iso);

    /* @S: the eval sink reached by concolic state.code, breakout constructed + fire-verified */
    char *ss = solve_json();
    printf("@SEC %s\n", ss);
    /* @S JS: single-quote-context breakout fire-verified. @S HTML: innerHTML sink fire-verified via Lexbor re-parse. */
    int s_eval = strstr(ss, "\"sink\":\"eval\"") && strstr(ss, "{state}.code") && strstr(ss, "';X9();//");
    int s_html = strstr(ss, "\"sink\":\"innerHTML\"") && strstr(ss, "{state}.html") && strstr(ss, "<svg onload=X9()>");
    int s_url = strstr(ss, "\"sink\":\"location\"") && strstr(ss, "{state}.next") && strstr(ss, "javascript:X9()");
    int s_ok = s_eval && s_html && s_url;

    printf("%s\n", (h_ok && s_ok)
        ? "PASS: @H merge AND @S eval + innerHTML + location — 3 sink contexts, all SEARCHED + fire-verified"
        : "FAIL: @H or @S incorrect");

    free(ss);
    free(js);
    solve_free();
    endpoint_free();

    flow_registry_free(ctx);
    JS_RunGC(rt);   /* collect flow-local garbage from the runs before teardown */
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    return (h_ok && s_ok) ? 0 : 1;
}
