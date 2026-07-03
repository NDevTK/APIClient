/* APIClient v2 host entry — the ONE scheduler.
 *
 * DESIGN (the ONE invariant): ONE persistent runtime, ONE top-level scheduler loop, EVERYTHING is a
 * flow the loop schedules. No phases, no separate grind, no second loop.
 *
 * Capabilities so far, each verified on the proven loop:
 *  - value-ordered (NON-FIFO) flow registry in scheduler-owned C memory; everything-a-flow (__fork).
 *  - PREEMPTION: a flow parks (await __yield) + resumes WITH STATE via quickjs-ng async-frame suspend.
 *  - fetch(url) host edge -> @H (the COMPUTED endpoint), all flow code runs in the ONE loop.
 *  - ORPHAN-INVOKE: force-invoke never-executed functions (JS_CollectOrphans) -> the UNUSED endpoints.
 *  - FORCED BRANCH-ARMS (this milestone): __branch() explores BOTH arms of a gated branch by
 *    decision-vector BFS — a flow re-runs its function with a forced-choice table; a new decision
 *    returns true for this flow and FORKS a sibling that replays the prefix then takes false. This
 *    surfaces the branch-gated (login/flag-gated) endpoints. Value-ordered, so productive paths first.
 *    (Auto-forking at OP_if on OPAQUE external input — so real bundles need no __branch — is the next
 *    engine capability; the decision-vector scheduling is proven here.)
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "quickjs.h"
#include "quickjs-libc.h"
#include <lexbor/html/html.h>
#include <lexbor/css/css.h>
#include <lexbor/selectors/selectors.h>
#include <lexbor/dom/dom.h>
#include <lexbor/url/url.h>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#define KEEP EMSCRIPTEN_KEEPALIVE   /* export qjs_init/step/teardown for the persistent-instance protocol */
#else
#define KEEP
#endif

/* ---- the ONE flow registry (scheduler-owned memory) --------------------------------
   A flow is a STARTER (a function to force-invoke, with a decision vector for its branch choices) or
   a RESUMER (the resolve fn of a parked flow's __yield promise). Both carry value-of-information. */
/* NO BOUNDS: the registry and the decision vector grow dynamically (until RAM/disk, the platform floor
   — the design's UNBOUNDED). No FLOW_MAX / DEC_MAX cap that would truncate distinct work the scheduler
   would otherwise reach. (Eviction of the cold/low-value tail to IDB is the further step; growth removes
   the artificial ceiling first.) */
typedef struct {
    JSValue handle;      /* the function (starter/suspended) or resolve fn (async __yield resume) */
    double val;          /* accumulated value-of-information (emits raise it) */
    int is_resume;       /* async __yield resume: JS_Call the resolve fn (not sync-preemptible) */
    signed char *dec; int dec_n;  /* per-flow decision vector (branch-arm BFS) */
    void *fs;            /* live heap frame if SUSPENDED mid-run (JS_FlowResume), else NULL */
    int saved_c;         /* per-flow branch cursor (g_c) snapshot, restored on resume */
    double cpu;          /* back-edge CPU ticks since last emit (WFQ decay; reset to 0 on emit) */
    int visits;          /* times scheduled (UCB/fairness explore term) */
    int orphan_idx;      /* cross-session locator: index in deterministic orphan collection (-1 = boot/yield, not park-replayable) */
} Flow;
static Flow   *g_reg = NULL;
static int     g_reg_n = 0, g_reg_cap = 0;
static int     g_running = 0;
static double  g_cur_val = 0;
static Flow   *g_cur_flow = NULL;   /* running flow (a stable local copy; its weight is read by the yield hook) */
static int     g_emit_total = 0;
static JSRuntime *g_rt = NULL;
/* Cross-session frontier (park/resume by REPLAY): deterministic orphan collection gives each function a
   stable index; a parked flow's recipe = (orphan_idx, decision-vector). g_quantum > 0 = the host's
   per-page CPU slice (cross-page fairness): after that many STARTERS the rest of the frontier is emitted
   as @PARK recipes and the run stops, resumable next session. 0 = unlimited (run to completion). */
static JSValue g_orphan_buf[4096];
static int     g_orphan_n = 0;
static int     g_cur_orphan_idx = -1;   /* running flow's orphan index (inherited by its branch siblings) */
static int     g_quantum = 0;
static long    g_work = 0;              /* flow DISPATCHES this run (starter OR resume) — the quantum's work unit */
static int     g_resume_mode = 0;       /* resuming a parked frontier: seed ONLY the recipes, not fresh orphans */
static uint32_t g_bundle_id = 0;        /* stable id of THIS document's own scripts (Lexbor DOM scan, not regex) — the frontier key */
static JSValue g_opaque = JS_UNDEFINED;   /* the OPAQUE sentinel: external input the tool must not concretely decide */

/* A URL/shape carries an opaque HOLE — "{}" (generic) or "{tag}" (source-tagged: {hash}/{search}) — iff it
   has a '{' followed by only lowercase letters then '}'. Such a URL is not concretely fetchable. Generalizes
   the old literal strstr("{}") checks so source-tagged holes are still recognized as opaque. */
static int has_hole(const char *s) {
    if (!s) return 0;
    for (const char *p = s; (p = strchr(p, '{')); p++) {
        const char *q = p + 1; while (*q >= 'a' && *q <= 'z') q++;
        if (*q == '}') return 1;
    }
    return 0;
}

/* decision-vector state for the RUNNING starter flow (branch-arm BFS) — grows unbounded */
static JSValue      g_cur_fn = JS_UNDEFINED;   /* the running starter's function (borrowed) so __branch can fork a sibling that re-runs it */
static signed char *g_dec = NULL;              /* working decision vector: forced prefix + this flow's chosen-true suffix */
static int          g_dec_cap = 0;
static int          g_dec_n = 0;               /* length of decisions made/forced so far */
static int          g_c = 0;                   /* cursor: next decision index __branch will consume */

static int g_dec_ensure(int n) {              /* grow g_dec to hold >= n decisions */
    if (n <= g_dec_cap) return 1;
    int nc = g_dec_cap ? g_dec_cap * 2 : 64; while (nc < n) nc *= 2;
    signed char *nd = (signed char *)realloc(g_dec, (size_t)nc);
    if (!nd) return 0;
    g_dec = nd; g_dec_cap = nc; return 1;
}

static int reg_add(JSContext *ctx, JSValue handle, double val, int is_resume, signed char *dec, int dec_n)
{
    if (g_reg_n >= g_reg_cap) {
        int nc = g_reg_cap ? g_reg_cap * 2 : 256;
        Flow *nr = (Flow *)realloc(g_reg, (size_t)nc * sizeof(Flow));
        if (!nr) { printf("@WHY {\"phase\":\"reg_oom\"}\n"); JS_FreeValue(ctx, handle); free(dec); return 0; }
        g_reg = nr; g_reg_cap = nc;
    }
    g_reg[g_reg_n].handle = handle; g_reg[g_reg_n].val = val; g_reg[g_reg_n].is_resume = is_resume;
    g_reg[g_reg_n].dec = dec; g_reg[g_reg_n].dec_n = dec_n;
    g_reg[g_reg_n].fs = NULL; g_reg[g_reg_n].saved_c = 0; g_reg[g_reg_n].cpu = 0; g_reg[g_reg_n].visits = 0;
    g_reg[g_reg_n].orphan_idx = -1;
    g_reg_n++; return 1;
}

/* Re-add a SUSPENDED flow (a full copy, fs retained) so it interleaves back into the ONE registry. */
static int reg_readd(JSContext *ctx, Flow f)
{
    if (g_reg_n >= g_reg_cap) {
        int nc = g_reg_cap ? g_reg_cap * 2 : 256;
        Flow *nr = (Flow *)realloc(g_reg, (size_t)nc * sizeof(Flow));
        if (!nr) { printf("@WHY {\"phase\":\"reg_oom\"}\n");
                   if (f.fs) JS_FlowFree(g_rt, f.fs); JS_FreeValue(ctx, f.handle); free(f.dec); return 0; }
        g_reg = nr; g_reg_cap = nc;
    }
    g_reg[g_reg_n++] = f; return 1;
}

/* __emit(tag): the ONLY progress signal — a real host edge (@H) surfaced. */
static JSValue js_emit(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    const char *s = argc > 0 ? JS_ToCString(ctx, argv[0]) : NULL;
    printf("@H %s\n", s ? s : "?"); fflush(stdout);
    if (s) JS_FreeCString(ctx, s);
    g_emit_total++;
    if (g_running) { g_cur_val += 1.0; if (g_cur_flow) { g_cur_flow->val = g_cur_val; g_cur_flow->cpu = 0; } }
    return JS_UNDEFINED;
}

static JSValue js_opaque_stub(JSContext *ctx, JSValueConst t, int c, JSValueConst *v);   /* fwd: opaque-returning host stub */
/* fromReply: a bridge-provided map { url -> concrete reply body text }. A real GET is fired by the
   TRUSTED offscreen (safeFetch, one-per-endpoint) and its body injected here so r.json()/r.text()
   return the CONCRETE server reply -> a reply field flowing into a downstream request param becomes a
   REAL example value instead of {}. Absent url -> opaque (the honest shape). */
static JSValue g_reply_table = JS_UNDEFINED;
static JSContext *g_ctx;   /* the run's context (defined = NULL below); fwd-declared for Lexbor callbacks */
/* ENDPOINT REGISTRY + IDENTITY: the engine ACCUMULATES every learned endpoint (method/url/params/
   headers/body) as a JS object here, and at finalize DEDUPS them (exact by method+hole-normalized url,
   then collapses a concrete instance into its shape with a path-param example) and emits the deduped
   set. Identity is the ENGINE's, not the host's (the JS mergeCallsites/dedupShapeConcrete were DELETED).
   The dedup runs on the engine's OWN quickjs (g_dedup_fn) — proven logic, executed in-engine, never a
   host context-switch. */
static JSValue g_endpoints = JS_UNDEFINED;   /* JS array of {method,url,params,headers,body} */
static JSValue g_dedup_fn = JS_UNDEFINED;    /* (eps) => deduped array, evaluated once at init */
static const char *DEDUP_JS =
"(function(eps){"
"  var HOLE=/\\{[a-z]*\\}/,HOLE_G=/\\{[a-z]*\\}/g,SEG_HOLE=/^\\{[a-z]*\\}$/;"
"  var hasHole=function(s){return HOLE.test(s||'');};"
"  var normHoles=function(s){return (s||'').replace(HOLE_G,'{}');};"
"  var pathSegs=function(u){var q=u.indexOf('?');var p=q>=0?u.slice(0,q):u;return {segs:p.split('/'),query:q>=0?u.slice(q):''};};"
"  for(var bi=0;bi<eps.length;bi++){var e=eps[bi];"
"    if(e.body){try{var bo=JSON.parse(e.body);"
"      if(bo&&typeof bo==='object'&&!Array.isArray(bo)){e.params=e.params||[];"
"        for(var bk in bo){var bv=bo[bk];"
"          var op=bv===null||bv==='{}'||(typeof bv==='object'&&bv&&!Array.isArray(bv)&&Object.keys(bv).length===0);"
"          var has=false;for(var qi=0;qi<e.params.length;qi++){if(e.params[qi].name===bk&&e.params[qi].location==='body'){has=true;break;}}"
"          if(!has)e.params.push({name:bk,location:'body',validValues:op?[]:[String(bv)]});"
"        }"
"      }"
"    }catch(_e){}}"
"  }"
"  var map=new Map();"
"  var mergeInto=function(e,s){"                                                 /* UNION s into e (same identity) */
"    var sp=s.params||[];e.params=e.params||[];"
"    for(var pi=0;pi<sp.length;pi++){var np=sp[pi],f=null;"
"      for(var ei=0;ei<e.params.length;ei++){if(e.params[ei].name===np.name&&e.params[ei].location===np.location){f=e.params[ei];break;}}"
"      if(!f){e.params.push(np);}else{var nv=np.validValues||[];f.validValues=f.validValues||[];for(var vi=0;vi<nv.length;vi++){if(f.validValues.indexOf(nv[vi])<0)f.validValues.push(nv[vi]);}}"
"    }"
"    if(s.headers){e.headers=e.headers||{};for(var hk in s.headers){if(!(hk in e.headers))e.headers[hk]=s.headers[hk];}}"
"    if(s.body&&!e.body)e.body=s.body;"
"  };"
"  for(var i=0;i<eps.length;i++){var s=eps[i];var k=(s.method||'GET')+' '+normHoles(s.url);if(!map.has(k))map.set(k,s);else mergeInto(map.get(k),s);}"
"  var arr=[];map.forEach(function(v){arr.push(v);});"
"  var shapes=arr.filter(function(e){return hasHole(e.url);});"
"  if(shapes.length){"
"    for(var ci=0;ci<arr.length;ci++){var c=arr[ci];if(hasHole(c.url))continue;"
"      for(var si=0;si<shapes.length;si++){var sh=shapes[si];"
"        if((sh.method||'GET')!==(c.method||'GET'))continue;"
"        var ss=pathSegs(sh.url),cs=pathSegs(c.url);"
"        if(ss.segs.length!==cs.segs.length||ss.query!==cs.query)continue;"
"        var ok=true,ex=[];"
"        for(var j=0;j<ss.segs.length;j++){"
"          if(SEG_HOLE.test(ss.segs[j])){if(cs.segs[j]&&!hasHole(cs.segs[j]))ex.push([j,cs.segs[j]]);else{ok=false;break;}}"
"          else if(ss.segs[j]!==cs.segs[j]){ok=false;break;}"
"        }"
"        if(ok&&ex.length){"
"          for(var e2=0;e2<ex.length;e2++){var idx=ex[e2][0],v=ex[e2][1];"
"            var pp=null,pl=sh.params||[];for(var pi=0;pi<pl.length;pi++){if(pl[pi].location==='path'&&pl[pi].name==='arg'+idx){pp=pl[pi];break;}}"
"            if(!pp){pp={name:'arg'+idx,location:'path',validValues:[]};(sh.params=sh.params||[]).push(pp);}"
"            if(pp.validValues.indexOf(v)<0)pp.validValues.push(v);"
"          }"
"          map.delete((c.method||'GET')+' '+c.url);break;"
"        }"
"      }"
"    }"
"  }"
"  var out=[];map.forEach(function(v){out.push(v);});return out;"
"})";
/* In-place fetch (pivot M2): a reply-consume (r.json()/r.text()) with no concrete body PARKS — an
   unresolved promise whose resolve fn is held here with the (concrete) url. qjs_step returns NEED_FETCH
   while any are pending; the offscreen safe-fetches and qjs_provide()s the body, resolving the promise
   so the flow resumes IN PLACE (no re-instantiate, no re-run). Node CLI resolves them opaque (shapes). */
typedef struct { JSValue rf; char *url; int is_json; } Pending;
static Pending *g_pending = NULL;
static int g_pending_n = 0, g_pending_cap = 0;
static void pending_add(JSContext *ctx, JSValue rf, const char *url, int is_json) {
    if (g_pending_n >= g_pending_cap) {
        int nc = g_pending_cap ? g_pending_cap * 2 : 32;
        Pending *n = realloc(g_pending, (size_t)nc * sizeof(Pending));
        if (!n) { JS_FreeValue(ctx, rf); return; }
        g_pending = n; g_pending_cap = nc;
    }
    g_pending[g_pending_n].rf = rf; g_pending[g_pending_n].url = url ? strdup(url) : NULL; g_pending[g_pending_n].is_json = is_json;
    g_pending_n++;
}
/* Chunk-load pending (M3): an injected <script src> to fetch + eval IN PLACE (no promise; nothing awaits
   it — like the browser's async script load). The offscreen provides the body; qjs_provide evals it. */
static char **g_chunk_pending = NULL;
static int g_chunk_n = 0, g_chunk_cap = 0;
static void chunk_pending_add(const char *url) {
    if (!url) return;
    for (int i = 0; i < g_chunk_n; i++) if (strcmp(g_chunk_pending[i], url) == 0) return;   /* dedup */
    if (g_chunk_n >= g_chunk_cap) { int nc = g_chunk_cap ? g_chunk_cap * 2 : 16; char **n = realloc(g_chunk_pending, (size_t)nc * sizeof(char *)); if (!n) return; g_chunk_pending = n; g_chunk_cap = nc; }
    g_chunk_pending[g_chunk_n++] = strdup(url);
}
/* wrap val in an already-RESOLVED promise (consumes val) so `await`/`.then` chains continue synchronously. */
static JSValue js_resolved(JSContext *ctx, JSValue val)
{
    JSValue rf[2]; JSValue promise = JS_NewPromiseCapability(ctx, rf);
    if (!JS_IsException(promise)) {
        JSValue rr = JS_Call(ctx, rf[0], JS_UNDEFINED, 1, &val); JS_FreeValue(ctx, rr);
        JS_FreeValue(ctx, rf[0]); JS_FreeValue(ctx, rf[1]);
    }
    JS_FreeValue(ctx, val);
    return promise;
}
/* Response body accessor (.json/.text/.blob/.arrayBuffer/.formData): the body is EXTERNAL INPUT ->
   OPAQUE, wrapped in a resolved promise so the fetch->reply->fetch chain CONTINUES and downstream
   endpoints surface as shapes (`/api/next/{}`). A real concrete reply is the host safe-fetch's job
   (fromReply, one-per-endpoint) — opaque here keeps the chain alive without inventing a value. */
static JSValue js_resp_body(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{ return js_resolved(ctx, JS_DupValue(ctx, g_opaque)); }
/* the concrete reply body injected onto this Response (fromReply), or JS_UNDEFINED */
static JSValue resp_body_str(JSContext *ctx, JSValueConst this_val) {
    JSValue b = JS_GetPropertyStr(ctx, this_val, "__body");
    if (JS_IsString(b)) return b;
    JS_FreeValue(ctx, b); return JS_UNDEFINED;
}
/* the bundle is CONSUMING this reply's body but we have no concrete one -> a real bounded GET to this
   (concrete) url is worth firing (the offscreen does it, one-per-endpoint). Emitted so the bridge fetches
   ONLY consumed-reply endpoints, not every GET (a blind GET can hit /logout, /delete?id=). */
static void reply_note_wanted(JSContext *ctx, JSValueConst this_val) {
    JSValue u = JS_GetPropertyStr(ctx, this_val, "url");
    const char *s = JS_IsString(u) ? JS_ToCString(ctx, u) : NULL;
    if (s && s[0] && !has_hole(s)) { printf("@REPLYWANT %s\n", s); fflush(stdout); }
    if (s) JS_FreeCString(ctx, s);
    JS_FreeValue(ctx, u);
}
/* parse a concrete reply body into the value json()/text() should resolve to */
static JSValue reply_value(JSContext *ctx, JSValueConst body_str, int is_json) {
    if (!is_json) return JS_DupValue(ctx, body_str);
    size_t len = 0; const char *s = JS_ToCStringLen(ctx, &len, body_str);
    JSValue parsed = s ? JS_ParseJSON(ctx, s, len, "<reply>") : JS_EXCEPTION;
    if (s) JS_FreeCString(ctx, s);
    if (JS_IsException(parsed)) { JSValue e = JS_GetException(ctx); JS_FreeValue(ctx, e); return JS_DupValue(ctx, g_opaque); }
    return parsed;   /* CONCRETE reply object -> its fields are real example values */
}
/* r.json()/r.text(): concrete body -> resolve it; else PARK (unresolved promise + pending) if the url is
   concrete (fetchable), so the offscreen provides the real reply IN PLACE; opaque if the url is a shape. */
static JSValue resp_consume(JSContext *ctx, JSValueConst this_val, int is_json) {
    JSValue body = resp_body_str(ctx, this_val);
    if (JS_IsString(body)) { JSValue v = reply_value(ctx, body, is_json); JS_FreeValue(ctx, body); return js_resolved(ctx, v); }
    JS_FreeValue(ctx, body);
    JSValue u = JS_GetPropertyStr(ctx, this_val, "url");
    const char *url = JS_IsString(u) ? JS_ToCString(ctx, u) : NULL;
    JSValue result;
    if (url && url[0] && !has_hole(url)) {   /* concrete url -> park for a real reply */
        JSValue rf[2]; JSValue promise = JS_NewPromiseCapability(ctx, rf);
        if (!JS_IsException(promise)) { pending_add(ctx, JS_DupValue(ctx, rf[0]), url, is_json); JS_FreeValue(ctx, rf[0]); JS_FreeValue(ctx, rf[1]); result = promise; }
        else result = js_resolved(ctx, JS_DupValue(ctx, g_opaque));
    } else {
        result = js_resolved(ctx, JS_DupValue(ctx, g_opaque));   /* shape url: not fetchable */
    }
    if (url) JS_FreeCString(ctx, url);
    JS_FreeValue(ctx, u);
    return result;
}
static JSValue js_resp_json(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) { return resp_consume(ctx, this_val, 1); }
static JSValue js_resp_text(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) { return resp_consume(ctx, this_val, 0); }
/* build a fetch Response whose identity is concrete (ok/status/url) but whose BODY is opaque. */
static JSValue make_response(JSContext *ctx, const char *url)
{
    JSValue resp = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, resp, "url", JS_NewString(ctx, url ? url : ""));
    JS_SetPropertyStr(ctx, resp, "ok", JS_TRUE);
    JS_SetPropertyStr(ctx, resp, "status", JS_NewInt32(ctx, 200));
    JS_SetPropertyStr(ctx, resp, "statusText", JS_NewString(ctx, "OK"));
    JS_SetPropertyStr(ctx, resp, "json", JS_NewCFunction(ctx, js_resp_json, "json", 0));
    JS_SetPropertyStr(ctx, resp, "text", JS_NewCFunction(ctx, js_resp_text, "text", 0));
    JS_SetPropertyStr(ctx, resp, "blob", JS_NewCFunction(ctx, js_resp_body, "blob", 0));
    JS_SetPropertyStr(ctx, resp, "arrayBuffer", JS_NewCFunction(ctx, js_resp_body, "arrayBuffer", 0));
    JS_SetPropertyStr(ctx, resp, "formData", JS_NewCFunction(ctx, js_resp_body, "formData", 0));
    {   /* headers.get(name) -> opaque (a response header is external input) */
        JSValue h = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, h, "get", JS_NewCFunction(ctx, js_opaque_stub, "get", 1));
        JS_SetPropertyStr(ctx, resp, "headers", h);
    }
    /* fromReply: inject the concrete reply body for THIS url (r.json()/r.text() then return real data) */
    if (url && JS_IsObject(g_reply_table)) {
        JSValue b = JS_GetPropertyStr(ctx, g_reply_table, url);
        if (JS_IsString(b)) JS_SetPropertyStr(ctx, resp, "__body", b);   /* consumes b */
        else JS_FreeValue(ctx, b);
    }
    return resp;
}
static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
/* Percent-decode a URL component (decodeURIComponent semantics: %XX -> byte; '+' stays '+'; a
   malformed % is left literal; control chars flattened to space so they can't break a line) into a
   NUL-terminated buffer. The ENGINE owns URL parsing — the host must never re-split the URL string. */
static void url_pct_decode(const char *s, size_t n, char *out, size_t outcap) {
    size_t o = 0;
    for (size_t i = 0; i < n && o + 1 < outcap; i++) {
        int hi, lo;
        if (s[i] == '%' && i + 2 < n
            && (hi = hexval(s[i+1])) >= 0 && (lo = hexval(s[i+2])) >= 0) {
            int c = (hi << 4) | lo; out[o++] = (c == '\n' || c == '\r') ? ' ' : (char)c; i += 2;
        } else {
            out[o++] = (s[i] == '\n' || s[i] == '\r') ? ' ' : s[i];
        }
    }
    out[o] = 0;
}
/* Build query-param objects ({name,location:"query",validValues:[value?]}) from a COMPUTED url onto the
   endpoint's params array. The engine owns URL parsing (Lexbor-canonical); the host never re-splits the
   URL string. A hole value ({search}) passes through literally (opacity marker, decoded downstream). */
static void build_query_params(JSContext *ctx, const char *url, JSValueConst params) {
    if (!url) return;
    const char *q = strchr(url, '?'); if (!q) return;
    q++;
    const char *end = strchr(q, '#'); if (!end) end = q + strlen(q);
    uint32_t idx = 0;
    for (const char *p = q; p < end; ) {
        const char *amp = memchr(p, '&', (size_t)(end - p)); if (!amp) amp = end;
        const char *eq = memchr(p, '=', (size_t)(amp - p));
        const char *ne = eq ? eq : amp;
        const char *vb = eq ? eq + 1 : amp;
        char nbuf[256], vbuf[512];
        url_pct_decode(p, (size_t)(ne - p), nbuf, sizeof nbuf);
        url_pct_decode(vb, (size_t)(amp - vb), vbuf, sizeof vbuf);
        if (nbuf[0]) {
            JSValue po = JS_NewObject(ctx);
            JS_SetPropertyStr(ctx, po, "name", JS_NewString(ctx, nbuf));
            JS_SetPropertyStr(ctx, po, "location", JS_NewString(ctx, "query"));
            JSValue vv = JS_NewArray(ctx);
            if (vbuf[0]) JS_SetPropertyUint32(ctx, vv, 0, JS_NewString(ctx, vbuf));
            JS_SetPropertyStr(ctx, po, "validValues", vv);
            JS_SetPropertyUint32(ctx, params, idx++, po);
        }
        p = (amp < end) ? amp + 1 : end;
    }
}
/* A JS string with control chars flattened to space (so an emitted line can't break), capped at cap. */
static JSValue js_str_flat(JSContext *ctx, const char *s, int cap) {
    char buf[1024]; int n = 0;
    for (const char *p = s; *p && n < cap && n < (int)sizeof(buf) - 1; p++, n++)
        buf[n] = (*p == '\n' || *p == '\r') ? ' ' : *p;
    buf[n] = 0;
    return JS_NewString(ctx, buf);
}
/* fetch(url): the moat's host edge. URL = whatever the bundle COMPUTED. ACCUMULATE the endpoint record
   (method/url/params/headers/body) into g_endpoints — identity/dedup runs in-engine at finalize — raise
   the flow's value (the WFQ progress signal), and return a resolved promise wrapping the Response. */
static JSValue js_fetch(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    const char *url = argc > 0 ? JS_ToCString(ctx, argv[0]) : NULL;
    /* HTTP method from the RequestInit (fetch(url,{method:'DELETE'})) — a big security signal (GET vs
       DELETE/POST). A concrete string only; opaque/missing options -> GET. */
    const char *method = NULL;
    if (argc > 1 && JS_IsObject(argv[1])) {
        JSValue m = JS_GetPropertyStr(ctx, argv[1], "method");
        if (JS_IsString(m)) method = JS_ToCString(ctx, m);   /* opaque options -> .method opaque (not a string) -> GET */
        JS_FreeValue(ctx, m);
    }
    if (!method && argc > 0 && JS_IsObject(argv[0])) {       /* fetch(new Request(url,{method})) -> read the Request's method */
        JSValue m = JS_GetPropertyStr(ctx, argv[0], "method");
        if (JS_IsString(m)) method = JS_ToCString(ctx, m);
        JS_FreeValue(ctx, m);
    }
    JSValue ep = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, ep, "method", JS_NewString(ctx, method ? method : "GET"));
    JS_SetPropertyStr(ctx, ep, "url", JS_NewString(ctx, url ? url : "?"));
    JS_SetPropertyStr(ctx, ep, "source", JS_NewString(ctx, "ast_analysis"));
    JSValue params = JS_NewArray(ctx);
    build_query_params(ctx, url, params);
    JS_SetPropertyStr(ctx, ep, "params", params);
    /* REQUIRED HEADERS + REQUEST BODY: part of the endpoint spec (replay). A plain-object `headers` ->
       ep.headers{name:value}; a POST/PATCH body (already stringified by the bundle; opaque fields -> {}
       shape) -> ep.body. Both control-flattened + capped so a value can't break the emitted line. */
    {
        JSValueConst init = (argc > 1 && JS_IsObject(argv[1])) ? argv[1] : ((argc > 0 && JS_IsObject(argv[0])) ? argv[0] : JS_UNDEFINED);
        if (JS_IsObject(init)) {
            JSValue hdrs = JS_GetPropertyStr(ctx, init, "headers");
            if (JS_IsObject(hdrs) && !JS_IsOpaque(hdrs)) {
                JSValue hobj = JS_NewObject(ctx); int any = 0;
                JSPropertyEnum *tab = NULL; uint32_t hn = 0;
                if (JS_GetOwnPropertyNames(ctx, &tab, &hn, hdrs, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) == 0) {
                    for (uint32_t hi = 0; hi < hn; hi++) {
                        const char *hk = JS_AtomToCString(ctx, tab[hi].atom);
                        JSValue hv = JS_GetProperty(ctx, hdrs, tab[hi].atom);
                        const char *hvs = JS_ToCString(ctx, hv);
                        if (hk && hvs) { JS_SetPropertyStr(ctx, hobj, hk, js_str_flat(ctx, hvs, 512)); any = 1; }
                        if (hk) JS_FreeCString(ctx, hk);
                        if (hvs) JS_FreeCString(ctx, hvs);
                        JS_FreeValue(ctx, hv);
                    }
                    JS_FreePropertyEnum(ctx, tab, hn);
                }
                if (any) JS_SetPropertyStr(ctx, ep, "headers", hobj); else JS_FreeValue(ctx, hobj);
            }
            JS_FreeValue(ctx, hdrs);
            JSValue body = JS_GetPropertyStr(ctx, init, "body");
            if (!JS_IsUndefined(body) && !JS_IsNull(body)) {
                const char *bs = JS_ToCString(ctx, body);
                if (bs && bs[0]) JS_SetPropertyStr(ctx, ep, "body", js_str_flat(ctx, bs, 600));
                if (bs) JS_FreeCString(ctx, bs);
            }
            JS_FreeValue(ctx, body);
        }
    }
    if (JS_IsArray(g_endpoints)) {
        uint32_t n = 0; JSValue lv = JS_GetPropertyStr(ctx, g_endpoints, "length"); JS_ToUint32(ctx, &n, lv); JS_FreeValue(ctx, lv);
        JS_SetPropertyUint32(ctx, g_endpoints, n, ep);   /* consumes ep */
    } else {
        JS_FreeValue(ctx, ep);
    }
    g_emit_total++;
    if (g_running) { g_cur_val += 1.0; if (g_cur_flow) { g_cur_flow->val = g_cur_val; g_cur_flow->cpu = 0; } }
    JSValue resp = make_response(ctx, url);
    if (url) JS_FreeCString(ctx, url);
    if (method) JS_FreeCString(ctx, method);
    return js_resolved(ctx, resp);
}
/* At finalize: DEDUP the accumulated endpoints in-engine (g_dedup_fn) and emit the whole structured
   result as ONE `@RESULT <json>` line (JSON.stringify — correct escaping, single line). The host does
   ONE JSON.parse and relays it: no @H/@P/@HDR/@BODY text protocol, no host-side re-parse, no host
   identity. Sinks/chunks/errors/park are added to the same object by their accumulate sites. */
static JSValue g_sinks = JS_UNDEFINED;        /* JS array of @S records */
static JSValue g_chunkurls = JS_UNDEFINED;    /* JS array of discovered external <script src> */
static JSValue g_whys = JS_UNDEFINED;         /* JS array of {context,message} zero-result/error reasons */
static JSValue g_park = JS_UNDEFINED;         /* JS array of "hash,decbits" frontier replay recipes */
static void arr_push_str(JSContext *ctx, JSValueConst arr, const char *s) {
    if (!JS_IsArray(arr) || !s) return;
    uint32_t n = 0; JSValue lv = JS_GetPropertyStr(ctx, arr, "length"); JS_ToUint32(ctx, &n, lv); JS_FreeValue(ctx, lv);
    JS_SetPropertyUint32(ctx, arr, n, JS_NewString(ctx, s));
}
static void emit_result(JSContext *ctx) {
    if (JS_IsUndefined(g_dedup_fn) || !JS_IsArray(g_endpoints)) return;
    JSValueConst args[1] = { g_endpoints };
    JSValue deduped = JS_Call(ctx, g_dedup_fn, JS_UNDEFINED, 1, args);
    if (JS_IsException(deduped)) { js_std_dump_error(ctx); deduped = JS_NewArray(ctx); }
    JSValue result = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, result, "fetchCallSites", deduped);                 /* consumes deduped */
    JS_SetPropertyStr(ctx, result, "securitySinks", JS_DupValue(ctx, g_sinks));
    JS_SetPropertyStr(ctx, result, "chunkUrls", JS_DupValue(ctx, g_chunkurls));
    JS_SetPropertyStr(ctx, result, "resolverErrors", JS_DupValue(ctx, g_whys));
    JS_SetPropertyStr(ctx, result, "_park", JS_DupValue(ctx, g_park));
    JS_SetPropertyStr(ctx, result, "_orphans", JS_NewInt32(ctx, g_orphan_n));
    JS_SetPropertyStr(ctx, result, "_emit", JS_NewInt32(ctx, g_emit_total));
    JSValue json = JS_JSONStringify(ctx, result, JS_UNDEFINED, JS_UNDEFINED);
    JS_FreeValue(ctx, result);
    if (JS_IsString(json)) { const char *js = JS_ToCString(ctx, json); if (js) { printf("@RESULT %s\n", js); JS_FreeCString(ctx, js); } }
    JS_FreeValue(ctx, json);
    fflush(stdout);
}

/* __branch(): a FORCED DECISION POINT (a gate on opaque external input). If the running flow's decision
   vector already fixes this point, replay it. Otherwise it's a NEW branch: FORK a sibling flow that
   re-runs the SAME function, replaying this flow's decisions so far then taking FALSE here; this flow
   takes TRUE (recorded so deeper new branches fork correctly). BFS over the decision tree -> both arms
   of every gate are explored, surfacing the branch-gated endpoints. */
/* branch_decide: the decision-vector fork logic (0/1). Called BOTH by __branch() (explicit) and by the
   engine's OP_if hook when a branch condition is OPAQUE (real bundles). Forced replay of this flow's
   decision prefix; a NEW decision forks the FALSE sibling (re-run the same function) and takes TRUE. */
static int branch_decide(JSContext *ctx)
{
    if (!g_running || JS_IsUndefined(g_cur_fn)) return 0;   /* only meaningful inside a starter flow */
    if (g_c < g_dec_n) return g_dec[g_c++] ? 1 : 0;         /* forced replay */
    if (!g_dec_ensure(g_c + 1)) return 1;                    /* only RAM/disk (the platform floor) bounds depth — not a cap */
    signed char *sib = (signed char *)malloc((size_t)(g_c + 1));
    if (sib) {
        for (int i = 0; i < g_c; i++) sib[i] = g_dec[i];
        sib[g_c] = 0;
        reg_add(ctx, JS_DupValue(ctx, g_cur_fn), g_cur_val, 0, sib, g_c + 1);
        g_reg[g_reg_n - 1].orphan_idx = g_cur_orphan_idx;   /* sibling = same function (same locator), different decisions */
    }
    g_dec[g_c] = 1; g_dec_n = g_c + 1; g_c++;
    return 1;
}
/* crypto.getRandomValues(arr): the spec FILLS + RETURNS the same typed array. The bytes are external
   randomness — nondeterministic, so filling them would (a) break replay soundness and (b) fabricate a
   concrete value. We can't store an opaque in a numeric typed array, so we leave it unmodified (its
   deterministic initial zeros) and return the SAME array, so `crypto.getRandomValues(new Uint8Array(n))`
   and the fill-then-read idiom both work without throwing. randomUUID (the URL-relevant one) is opaque. */
static JSValue js_crypto_getrandom(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{ return argc >= 1 ? JS_DupValue(ctx, argv[0]) : JS_DupValue(ctx, g_opaque); }
static int host_opaque(JSValueConst v);                                        /* defined below */
static void emit_sink(JSContext *ctx, const char *sink, JSValueConst tainted);  /* defined below */
/* setTimeout/setInterval/requestAnimationFrame(cb, ...): a deferred callback is NOT a wait on real time —
   it is just another BFS FLOW. Register cb in the ONE scheduler (reg_add) so it is driven, ordered, and
   starved by the same WFQ as every other flow (the whole point: bundles that defer init in a timer still
   get explored). Return an opaque timer id; the clear/cancel variants are no-ops (the WFQ starves it anyway). */
static JSValue js_set_timer(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    if (argc >= 1 && JS_IsFunction(ctx, argv[0]))
        reg_add(ctx, JS_DupValue(ctx, argv[0]), g_running ? g_cur_val : 1.0, 0, NULL, 0);
    else if (argc >= 1 && host_opaque(argv[0]))
        emit_sink(ctx, "setTimeout", argv[0]);   /* setTimeout(taintedString) executes the string as code -> eval sink */
    return JS_DupValue(ctx, g_opaque);
}
/* localStorage/sessionStorage.getItem(k): stored data is EXTERNAL INPUT (a token/flag put there earlier or
   by another origin's code) -> OPAQUE (feeds auth headers/branches opaquely, replay-sound). set/remove/clear
   are no-ops (writes don't drive discovery). */
static JSValue js_storage_get(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{ return JS_DupValue(ctx, g_opaque); }
/* structuredClone(x): deep-clone is identity for forced-exec purposes (shape/opacity carry through). */
static JSValue js_structured_clone(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{ return argc >= 1 ? JS_DupValue(ctx, argv[0]) : JS_UNDEFINED; }

/* URL / URLSearchParams: endpoint construction. `new URL(path, base).href|pathname` is how a huge share of
   bundles build request URLs — undefined `URL` = ReferenceError = the endpoint is lost. A CONCRETE input is
   resolved by the REAL vendored LEXBOR URL parser (bind-before-build: existing Lexbor module, never a
   hand-rolled string resolver). An OPAQUE input (external-input-tainted, or a shape with {} holes Lexbor
   can't parse) -> OPAQUE, so its shape flows through untouched (the endpoint is learned as its shape) and
   the tool never concretely decides external input. searchParams.get is OPAQUE (query values = external). */
static const char *g_origin;   /* defined below; forward for the URL helpers */
static JSValue js_opaque(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
static JSValue js_noop(JSContext *ctx, JSValueConst t, int c, JSValueConst *v);
/* Resolve a URL with the vendored LEXBOR URL module (the real WHATWG URL Standard parser) — never a
   hand-rolled string resolver. Returns the serialized absolute href (malloc'd; caller frees) or NULL on a
   parse failure (-> the caller yields opaque, never an invented value). */
struct url_ser_buf { char *s; size_t n, cap; };
static lxb_status_t url_ser_cb(const lxb_char_t *data, size_t len, void *cbctx) {
    struct url_ser_buf *b = cbctx;
    if (b->n + len + 1 > b->cap) { size_t nc = (b->n + len + 1) * 2 + 64; char *ns = realloc(b->s, nc); if (!ns) return LXB_STATUS_ERROR_MEMORY_ALLOCATION; b->s = ns; b->cap = nc; }
    memcpy(b->s + b->n, data, len); b->n += len; b->s[b->n] = 0;
    return LXB_STATUS_OK;
}
static char *url_resolve(const char *input, const char *base) {
    lxb_url_parser_t *p = lxb_url_parser_create();
    if (!p || lxb_url_parser_init(p, NULL) != LXB_STATUS_OK) { if (p) lxb_url_parser_destroy(p, true); return NULL; }
    lxb_url_t *bu = (base && base[0]) ? lxb_url_parse(p, NULL, (const lxb_char_t *)base, strlen(base)) : NULL;
    lxb_url_t *u = lxb_url_parse(p, bu, (const lxb_char_t *)(input ? input : ""), input ? strlen(input) : 0);
    char *out = NULL;
    if (u) { struct url_ser_buf b = {0}; if (lxb_url_serialize(u, url_ser_cb, &b, false) == LXB_STATUS_OK) out = b.s; else free(b.s); }
    lxb_url_parser_destroy(p, true);   /* frees bu, u, and internal buffers */
    return out;
}
static void url_set(JSContext *ctx, JSValue o, const char *k, const char *s, size_t n) {
    JS_SetPropertyStr(ctx, o, k, JS_NewStringLen(ctx, s, n));
}
static JSValue js_url_tostring(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{ return JS_GetPropertyStr(ctx, this_val, "href"); }
static JSValue js_searchparams_get(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{ return JS_DupValue(ctx, g_opaque); }   /* query values are external input -> opaque */
static JSValue js_url_ctor(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv) {
    /* opaque (external-input-tainted) URL -> return the INPUT opaque so its SHAPE flows through unchanged
       (never concretely resolved — RUN-DON'T-MATCH). A concrete input is resolved by the REAL Lexbor parser. */
    if (argc >= 1 && JS_IsOpaque(argv[0])) return JS_DupValue(ctx, argv[0]);
    const char *input = argc >= 1 ? JS_ToCString(ctx, argv[0]) : NULL;
    const char *base  = argc >= 2 && JS_IsString(argv[1]) ? JS_ToCString(ctx, argv[1]) : NULL;
    char *resolved = (input && !has_hole(input)) ? url_resolve(input, base ? base : g_origin) : NULL;
    JSValue shaped = (input && has_hole(input)) ? JS_NewOpaqueShaped(ctx, input) : JS_UNDEFINED;  /* {}-shape string -> keep shape */
    if (input) JS_FreeCString(ctx, input);
    if (base) JS_FreeCString(ctx, base);
    if (!resolved) return JS_IsUndefined(shaped) ? JS_DupValue(ctx, g_opaque) : shaped;   /* shape/parse-fail -> opaque, never invent */
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "href", JS_NewString(ctx, resolved));
    /* Extract components from Lexbor's CANONICAL output (scheme://host/path?query#frag) — trivial split of a
       spec-parsed string, NOT a resolver reimplementation (Lexbor did the resolution). */
    const char *scol = strstr(resolved, "://");
    const char *host = scol ? scol + 3 : resolved;
    url_set(ctx, o, "protocol", resolved, scol ? (size_t)(scol - resolved) + 1 : 0);
    const char *pe = host; while (*pe && *pe != '/' && *pe != '?' && *pe != '#') pe++;
    url_set(ctx, o, "host", host, (size_t)(pe - host));
    url_set(ctx, o, "hostname", host, (size_t)(pe - host));
    url_set(ctx, o, "origin", resolved, (size_t)(pe - resolved));
    const char *path = pe; const char *q = strchr(path, '?'); const char *hsh = strchr(path, '#');
    const char *pend = q ? q : (hsh ? hsh : path + strlen(path));
    url_set(ctx, o, "pathname", *path ? path : "/", *path ? (size_t)(pend - path) : 1);
    if (q) { const char *qe = hsh ? hsh : q + strlen(q); url_set(ctx, o, "search", q, (size_t)(qe - q)); }
    else JS_SetPropertyStr(ctx, o, "search", JS_NewString(ctx, ""));
    if (hsh) JS_SetPropertyStr(ctx, o, "hash", JS_NewString(ctx, hsh)); else JS_SetPropertyStr(ctx, o, "hash", JS_NewString(ctx, ""));
    JS_SetPropertyStr(ctx, o, "port", JS_NewString(ctx, ""));
    free(resolved);
    JSValue sp = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, sp, "get", JS_NewCFunction(ctx, js_searchparams_get, "get", 1));
    JS_SetPropertyStr(ctx, sp, "getAll", JS_NewCFunction(ctx, js_searchparams_get, "getAll", 1));
    JS_SetPropertyStr(ctx, sp, "has", JS_NewCFunction(ctx, js_searchparams_get, "has", 1));
    JS_SetPropertyStr(ctx, sp, "toString", JS_NewCFunction(ctx, js_opaque, "toString", 0));
    JS_SetPropertyStr(ctx, o, "searchParams", sp);
    JS_SetPropertyStr(ctx, o, "toString", JS_NewCFunction(ctx, js_url_tostring, "toString", 0));
    JS_SetPropertyStr(ctx, o, "toJSON", JS_NewCFunction(ctx, js_url_tostring, "toJSON", 0));
    return o;
}
/* new Request(input, init): fetch(new Request(url,{method})) is common. Resolve the url (shape-aware, like
   URL), expose .url/.method/.headers and toString->url so fetch(req) reads the endpoint. */
static JSValue js_request_ctor(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv) {
    /* opaque input -> return the input opaque (url shape flows); concrete -> Lexbor-resolved url. */
    if (argc >= 1 && JS_IsOpaque(argv[0])) return JS_DupValue(ctx, argv[0]);
    const char *input = argc >= 1 ? JS_ToCString(ctx, argv[0]) : NULL;
    char *resolved = (input && !has_hole(input)) ? url_resolve(input, g_origin) : NULL;
    JSValue rshaped = (input && has_hole(input)) ? JS_NewOpaqueShaped(ctx, input) : JS_UNDEFINED;
    if (input) JS_FreeCString(ctx, input);
    if (!resolved) return JS_IsUndefined(rshaped) ? JS_DupValue(ctx, g_opaque) : rshaped;
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "url", JS_NewString(ctx, resolved));
    JS_SetPropertyStr(ctx, o, "href", JS_NewString(ctx, resolved));   /* toString reads href -> fetch(req) sees the url */
    free(resolved);
    JSValue method = JS_UNDEFINED;
    if (argc >= 2 && JS_IsObject(argv[1])) method = JS_GetPropertyStr(ctx, argv[1], "method");
    JS_SetPropertyStr(ctx, o, "method", JS_IsString(method) ? method : JS_NewString(ctx, "GET"));
    if (!JS_IsString(method)) JS_FreeValue(ctx, method);
    JS_SetPropertyStr(ctx, o, "toString", JS_NewCFunction(ctx, js_url_tostring, "toString", 0));
    return o;
}
/* A generic Web object whose reads are opaque (external input) and whose writes are no-ops: Headers,
   FormData, Blob, Response body, AbortController.signal, TextEncoder/Decoder. Prevents the ReferenceError
   that would kill the flow, and keeps any value read out of it OPAQUE (never a fabricated concrete). */
static JSValue js_webobj_ctor(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv) {
    JSValue o = JS_NewObject(ctx);
    const char *op[] = { "get", "getAll", "has", "keys", "values", "entries", "forEach", "encode", "decode",
                         "text", "json", "arrayBuffer", "blob", "formData", "clone", "slice", "getReader" };
    for (size_t i = 0; i < sizeof op / sizeof op[0]; i++)
        JS_SetPropertyStr(ctx, o, op[i], JS_NewCFunction(ctx, js_opaque, op[i], 1));
    const char *np[] = { "set", "append", "delete", "abort", "add", "addEventListener" };
    for (size_t i = 0; i < sizeof np / sizeof np[0]; i++)
        JS_SetPropertyStr(ctx, o, np[i], JS_NewCFunction(ctx, js_noop, np[i], 2));
    JS_SetPropertyStr(ctx, o, "signal", JS_DupValue(ctx, g_opaque));   /* AbortController.signal */
    JS_SetPropertyStr(ctx, o, "toString", JS_NewCFunction(ctx, js_opaque, "toString", 0));
    return o;
}
/* new URLSearchParams(init): .get/getAll/has -> opaque (external input); toString -> opaque. */
static JSValue js_searchparams_ctor(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv) {
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "get", JS_NewCFunction(ctx, js_searchparams_get, "get", 1));
    JS_SetPropertyStr(ctx, o, "getAll", JS_NewCFunction(ctx, js_searchparams_get, "getAll", 1));
    JS_SetPropertyStr(ctx, o, "has", JS_NewCFunction(ctx, js_searchparams_get, "has", 1));
    JS_SetPropertyStr(ctx, o, "append", JS_NewCFunction(ctx, js_noop, "append", 2));
    JS_SetPropertyStr(ctx, o, "set", JS_NewCFunction(ctx, js_noop, "set", 2));
    JS_SetPropertyStr(ctx, o, "toString", JS_NewCFunction(ctx, js_opaque, "toString", 0));
    return o;
}
static JSValue js_branch(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{ return branch_decide(ctx) ? JS_TRUE : JS_FALSE; }

/* __opaque(): return the OPAQUE sentinel — external input the tool must not concretely decide. A branch
   on it (if(__opaque())) auto-forks BOTH arms via the engine OP_if hook, no explicit __branch needed. */
static JSValue js_opaque(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{ return JS_DupValue(ctx, g_opaque); }
/* Minimal host-edge stubs for a browser bundle: a no-op (addEventListener etc — the handler stays a
   never-fired function, so orphan-invoke drives it), and an opaque-returning stub (DOM reads that are
   external input the tool must not concretely decide). A missing capability is a missing stub, never a
   parallel resolver — the real Lexbor DOM replaces these when the host is wired. */
static JSValue js_noop(JSContext *ctx, JSValueConst t, int c, JSValueConst *v) { return JS_UNDEFINED; }
static JSValue js_opaque_stub(JSContext *ctx, JSValueConst t, int c, JSValueConst *v) { return JS_DupValue(ctx, g_opaque); }
/* addEventListener(type, handler): a registered handler that NEVER FIRES is exactly the unused surface —
   keep it reachable (in g_handlers) so orphan-invoke drives it and surfaces its gated endpoints. */
static JSValue g_handlers = JS_UNDEFINED;
static int g_handler_n = 0;
/* Handlers registered for 'message' (postMessage). Driven with a synthetic MessageEvent whose .data is
   the source-tagged opaque {pm}, so a postMessage-XSS sink reports {pm} and the PoC assembler builds a
   postMessage-delivered PoC. Borrowed refs (also held live in g_handlers). */
static void *g_msg_handlers[128];
static int g_msg_handler_n = 0;
static JSValue g_msg_event = JS_UNDEFINED;   /* synthetic MessageEvent: { data: opaque("{pm}"), origin, source } */
static JSValue js_add_listener(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    JSValueConst h = (argc >= 2) ? argv[1] : (argc >= 1 ? argv[0] : JS_UNDEFINED);
    if (JS_IsFunction(ctx, h) && !JS_IsUndefined(g_handlers)) {
        JS_SetPropertyUint32(ctx, g_handlers, (uint32_t)g_handler_n++, JS_DupValue(ctx, h));
        const char *type = argc >= 2 ? JS_ToCString(ctx, argv[0]) : NULL;   /* addEventListener(type, handler) */
        if (type && strcmp(type, "message") == 0 && g_msg_handler_n < 128)
            g_msg_handlers[g_msg_handler_n++] = JS_VALUE_GET_PTR(h);
        if (type) JS_FreeCString(ctx, type);
    }
    return JS_UNDEFINED;
}
static int is_msg_handler(JSValueConst h) {
    void *p = JS_VALUE_GET_PTR(h);
    for (int i = 0; i < g_msg_handler_n; i++) if (g_msg_handlers[i] == p) return 1;
    return 0;
}

/* The page's OWN identity (principal) is CONCRETE for URL building — location.origin/protocol/host/
   hostname/port/pathname/href are REAL (a bundle does `location.origin + '/api/...'`; opaque here would
   yield a "{}"-shaped garbage URL and lose the endpoint). Only the EXTERNAL-INPUT parts — search/hash —
   stay OPAQUE (must never force a branch), yet carry a concrete example when page state already has one.
   The host injects the real principal at wire time; g_origin is the node-harness placeholder. */
static const char *g_origin   = "https://app.example.com";   /* host-injected real page principal (argv[3]); placeholder for node tests */
static const char *g_protocol = "https:";
static const char *g_host     = "app.example.com";
/* Split a real origin ("https://localhost:8765") into protocol + host for the concrete location.*. */
static void set_origin(const char *origin) {
    static char protobuf[64], hostbuf[256];
    const char *p;
    if (!origin || !origin[0]) return;
    g_origin = origin;
    p = strstr(origin, "://");
    if (!p) return;
    { size_t plen = (size_t)(p - origin) + 1; if (plen < sizeof protobuf) { memcpy(protobuf, origin, plen); protobuf[plen] = 0; g_protocol = protobuf; } }
    { const char *h = p + 3; size_t hlen = strlen(h); const char *slash = strchr(h, '/'); if (slash) hlen = (size_t)(slash - h);
      if (hlen < sizeof hostbuf) { memcpy(hostbuf, h, hlen); hostbuf[hlen] = 0; g_host = hostbuf; } }
}
static JSValue make_location(JSContext *ctx)
{
    JSValue loc = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, loc, "origin",   JS_NewString(ctx, g_origin));
    JS_SetPropertyStr(ctx, loc, "protocol", JS_NewString(ctx, g_protocol));
    JS_SetPropertyStr(ctx, loc, "host",     JS_NewString(ctx, g_host));
    JS_SetPropertyStr(ctx, loc, "hostname", JS_NewString(ctx, g_host));
    JS_SetPropertyStr(ctx, loc, "port",     JS_NewString(ctx, ""));
    JS_SetPropertyStr(ctx, loc, "pathname", JS_NewString(ctx, "/"));
    JS_SetPropertyStr(ctx, loc, "href",     JS_NewString(ctx, g_origin));   /* concrete base for new URL(path, href) */
    JS_SetPropertyStr(ctx, loc, "search",   JS_NewOpaqueShaped(ctx, "{search}"));  /* external input: opaque, source-tagged (PoC delivery = victim?...) */
    JS_SetPropertyStr(ctx, loc, "hash",     JS_NewOpaqueShaped(ctx, "{hash}"));    /* external input: opaque, source-tagged (PoC delivery = victim#...) */
    return loc;
}

/* ── Real DOM (Lexbor) ───────────────────────────────────────────────────────────
   The page's own structure/config is CONCRETE (a bundle reads `#cfg[data-api]` and builds a real
   URL); only external-input values stay opaque. document.* is backed by a live Lexbor DOM parsed from
   the page HTML, NOT a bridge-side scrape — so it can carry real attribute values now, and become
   per-flow COW state + intercept dynamic script injection (steps C/D). */
static lxb_html_document_t *g_dom = NULL;   /* parser + selectors are per-call (reuse corrupts the next query) */
static JSClassID g_el_class_id;

static JSValue el_wrap(JSContext *ctx, lxb_dom_element_t *el) {
    if (!el) return JS_NULL;
    JSValue o = JS_NewObjectClass(ctx, g_el_class_id);
    if (JS_IsException(o)) return o;
    JS_SetOpaque(o, el);
    return o;
}
struct sel_ctx { lxb_dom_element_t *first; };
static lxb_status_t sel_first_cb(lxb_dom_node_t *node, lxb_css_selector_specificity_t s, void *vp) {
    struct sel_ctx *c = vp; (void)s;
    if (!c->first) c->first = lxb_dom_interface_element(node);
    return LXB_STATUS_OK;   /* let the traversal COMPLETE so g_sel resets cleanly for the next find
                               (returning STOP mid-walk leaves internal state that breaks reuse) */
}
/* Find the first element matching a CSS selector, searching the whole parsed document. Fresh selectors
   object + cleaned parser per call — reusing them across finds carries internal state that breaks the
   next query (2nd querySelector returned null). The matched element is owned by g_dom, so tearing down
   the per-call selectors/list doesn't free it. */
static lxb_dom_element_t *dom_select_first(const char *sel, size_t len) {
    if (!g_dom) return NULL;
    lxb_css_parser_t *p = lxb_css_parser_create();
    if (!p || lxb_css_parser_init(p, NULL) != LXB_STATUS_OK) { if (p) lxb_css_parser_destroy(p, true); return NULL; }
    lxb_css_selector_list_t *list = lxb_css_selectors_parse(p, (const lxb_char_t *)sel, len);
    if (!list) { lxb_css_parser_destroy(p, true); return NULL; }
    lxb_selectors_t *s = lxb_selectors_create();
    if (!s || lxb_selectors_init(s) != LXB_STATUS_OK) { if (s) lxb_selectors_destroy(s, true); lxb_css_parser_destroy(p, true); return NULL; }
    struct sel_ctx c = { NULL };
    lxb_selectors_find(s, lxb_dom_interface_node(g_dom), list, sel_first_cb, &c);
    lxb_selectors_destroy(s, true);
    lxb_css_parser_destroy(p, true);   /* frees the list too (parser owns it); c.first lives in g_dom */
    return c.first;
}
static JSValue js_el_getAttribute(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    lxb_dom_element_t *el = JS_GetOpaque(this_val, g_el_class_id);
    if (!el || argc < 1) return JS_NULL;
    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_NULL;
    size_t vlen = 0;
    const lxb_char_t *v = lxb_dom_element_get_attribute(el, (const lxb_char_t *)name, strlen(name), &vlen);
    JS_FreeCString(ctx, name);
    return v ? JS_NewStringLen(ctx, (const char *)v, vlen) : JS_NULL;   /* REAL attribute value (concrete) */
}
static JSValue js_el_querySelector(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (argc < 1) return JS_NULL;
    const char *s = JS_ToCString(ctx, argv[0]); if (!s) return JS_NULL;
    lxb_dom_element_t *el = dom_select_first(s, strlen(s));   /* NB: document-scoped for now, not subtree */
    JS_FreeCString(ctx, s);
    return el_wrap(ctx, el);
}
static JSValue js_el_textContent(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    lxb_dom_element_t *el = JS_GetOpaque(this_val, g_el_class_id);
    if (!el) return JS_NULL;
    size_t len = 0;
    lxb_char_t *txt = lxb_dom_node_text_content(lxb_dom_interface_node(el), &len);
    if (!txt) return JS_NewString(ctx, "");
    JSValue r = JS_NewStringLen(ctx, (const char *)txt, len);
    lxb_dom_document_destroy_text(lxb_dom_interface_node(el)->owner_document, txt);
    return r;
}
/* ── Per-flow DOM COW isolation ──────────────────────────────────────────────────
   DOM mutations (attribute set, node insert) by a FORCED flow must not leak to sibling flows — the
   same invariant as the JS-heap COW. A host-side undo log records the inverse of each mutation while
   capture is active (during flow exploration, NOT boot which builds the baseline); dom_revert replays
   it to restore the post-boot DOM baseline, called alongside JS_CowRevert before each starter. */
typedef struct { int kind; lxb_dom_element_t *el; char *name; lxb_char_t *old; size_t old_len; int had; lxb_dom_node_t *node; } DomUndo;
static DomUndo *g_dom_undo = NULL;
static int g_dom_undo_n = 0, g_dom_undo_cap = 0, g_dom_capture = 0;
static void dom_undo_push(DomUndo u) {
    if (g_dom_undo_n >= g_dom_undo_cap) {
        int nc = g_dom_undo_cap ? g_dom_undo_cap * 2 : 64;
        DomUndo *n = realloc(g_dom_undo, (size_t)nc * sizeof(DomUndo)); if (!n) return; g_dom_undo = n; g_dom_undo_cap = nc;
    }
    g_dom_undo[g_dom_undo_n++] = u;
}
static void dom_attr_capture(lxb_dom_element_t *el, const char *name) {
    if (!g_dom_capture) return;
    size_t vl = 0; const lxb_char_t *cur = lxb_dom_element_get_attribute(el, (const lxb_char_t *)name, strlen(name), &vl);
    DomUndo u; memset(&u, 0, sizeof u);
    u.kind = 0; u.el = el; u.name = strdup(name); u.had = cur ? 1 : 0;
    if (cur) { u.old = malloc(vl ? vl : 1); if (u.old) { memcpy(u.old, cur, vl); u.old_len = vl; } }
    dom_undo_push(u);
}
static void dom_insert_capture(lxb_dom_node_t *node) {
    if (!g_dom_capture) return;
    DomUndo u; memset(&u, 0, sizeof u); u.kind = 1; u.node = node; dom_undo_push(u);
}
/* ── The SECURITY view (@S): the SECOND equal output of the one run ──────────────
   An XSS/injection SINK reached by OPAQUE (external-input-tainted) data is a security finding, weighted
   equally with @H. Opacity is the taint we already track: v is tainted iff it IS the opaque sentinel. */
static int host_opaque(JSValueConst v) {
    return JS_IsOpaque(v);   /* any shape-carrying opaque (taint), not just the default sentinel */
}
/* Emit an @S finding with the tainted value's real SHAPE (the concrete/opaque interleaving the PoC needs:
   `'<h1>'+hash` sinks the value opaque("<h1>{}"), so the sink reports `<h1>{}`, not a blind `{}`). The
   shape is the opaque's payload — read via JS_ToCString (ToString(opaque) returns its shape). Control
   chars are folded to spaces so the one-line @S protocol stays parseable. */
static void emit_sink(JSContext *ctx, const char *sink, JSValueConst tainted) {
    const char *sh = JS_ToCString(ctx, tainted);
    JSValue shape = sh ? js_str_flat(ctx, sh, 512) : JS_NewString(ctx, "{}");
    if (sh) JS_FreeCString(ctx, sh);
    const char *shs = JS_ToCString(ctx, shape);
    JSValue rec = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, rec, "type", JS_NewString(ctx, sink));
    JS_SetPropertyStr(ctx, rec, "sink", JS_NewString(ctx, sink));
    JS_SetPropertyStr(ctx, rec, "taint", JS_NewString(ctx, "opaque"));
    JS_SetPropertyStr(ctx, rec, "shape", JS_NewString(ctx, shs ? shs : "{}"));
    { char ev[600]; snprintf(ev, sizeof ev, "%s %s", sink, shs ? shs : "{}"); JS_SetPropertyStr(ctx, rec, "evidence", JS_NewString(ctx, ev)); }
    JS_SetPropertyStr(ctx, rec, "source", JS_NewString(ctx, "ast_analysis"));
    if (shs) JS_FreeCString(ctx, shs);
    JS_FreeValue(ctx, shape);
    if (JS_IsArray(g_sinks)) { uint32_t n=0; JSValue lv=JS_GetPropertyStr(ctx,g_sinks,"length"); JS_ToUint32(ctx,&n,lv); JS_FreeValue(ctx,lv); JS_SetPropertyUint32(ctx, g_sinks, n, rec); }
    else JS_FreeValue(ctx, rec);
    g_emit_total++;
    if (g_running && g_cur_flow) { g_cur_flow->val += 1.0; g_cur_flow->cpu = 0; }   /* @S raises value like @H (WFQ prioritizes sink-reaching flows) */
}
/* Concatenated taint IS now caught: opaque-with-provenance keeps `'<h1>'+taint` opaque (shape "<h1>{}"),
   so host_opaque() sees it at the sink and the shape gives the PoC its interleaving. The remaining PoC
   work is source-tagging the hole ({hash}/{search}/{pm}) + the transform chain for filtered flows. */
static void dom_revert(void) {   /* restore the DOM to the post-boot baseline (reverse order) */
    for (int i = g_dom_undo_n - 1; i >= 0; i--) {
        DomUndo *u = &g_dom_undo[i];
        if (u->kind == 0) {   /* attribute: restore old value, or remove if it didn't exist */
            if (u->had && u->old) lxb_dom_element_set_attribute(u->el, (const lxb_char_t *)u->name, strlen(u->name), u->old, u->old_len);
            else lxb_dom_element_remove_attribute(u->el, (const lxb_char_t *)u->name, strlen(u->name));
            free(u->name); free(u->old);
        } else if (u->kind == 1) {   /* inserted node: detach it */
            lxb_dom_node_remove(u->node);
        }
    }
    g_dom_undo_n = 0;
}

static int el_is_script(lxb_dom_element_t *el) {
    size_t nl = 0; const lxb_char_t *nm = lxb_dom_element_qualified_name(el, &nl);
    return nm && nl == 6 && memcmp(nm, "script", 6) == 0;
}
/* An inserted <script> with a src is a chunk LOAD (the URL may be JS-computed): surface it. */
static void script_maybe_load(lxb_dom_element_t *el) {
    if (!el_is_script(el)) return;
    size_t sl = 0; const lxb_char_t *src = lxb_dom_element_get_attribute(el, (const lxb_char_t *)"src", 3, &sl);
    if (src && sl) {
        char *u = strndup((const char *)src, sl);
        if (u) { arr_push_str(g_ctx, g_chunkurls, u); if (!has_hole(u)) chunk_pending_add(u); free(u); }   /* -> chunkUrls + fetch in place */
    }
}
/* dynamic import(specifier): force-fetch the ESM chunk in place (like a browser lazy-load, forced). Make the
   concrete specifier a pending chunk (fetched + eval'd; its functions become orphans the scheduler drives)
   and surface @CHUNK. The module namespace is OPAQUE (forced-exec runs the CODE, not the real exports). */
static JSValue host_dyn_import(JSContext *ctx, const char *specifier) {
    if (specifier && specifier[0] && !has_hole(specifier)) {
        arr_push_str(ctx, g_chunkurls, specifier);   /* -> @RESULT.chunkUrls */
        char *u = strdup(specifier);
        if (u) { chunk_pending_add(u); free(u); }
    }
    return JS_DupValue(ctx, g_opaque);
}
static int is_sink_attr(const char *n) {   /* attributes where tainted data is an XSS/redirect sink */
    return n && (strncmp(n, "on", 2) == 0 || strcmp(n, "href") == 0 || strcmp(n, "src") == 0 ||
                 strcmp(n, "action") == 0 || strcmp(n, "formaction") == 0 || strcmp(n, "srcdoc") == 0);
}
static JSValue js_el_setAttribute(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    lxb_dom_element_t *el = JS_GetOpaque(this_val, g_el_class_id);
    if (!el || argc < 2) return JS_UNDEFINED;
    const char *name = JS_ToCString(ctx, argv[0]);
    if (name && host_opaque(argv[1]) && is_sink_attr(name)) emit_sink(ctx, "setAttribute", argv[1]);   /* @S: tainted into an on-handler or url attr */
    const char *val = JS_ToCString(ctx, argv[1]);
    if (name && val) { dom_attr_capture(el, name); lxb_dom_element_set_attribute(el, (const lxb_char_t *)name, strlen(name), (const lxb_char_t *)val, strlen(val)); }
    if (name) JS_FreeCString(ctx, name);
    if (val) JS_FreeCString(ctx, val);
    return JS_UNDEFINED;
}
/* el.insertAdjacentHTML(pos, html) — tainted html is a direct XSS sink. */
static JSValue js_el_insertAdjacentHTML(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (argc >= 2 && host_opaque(argv[1])) emit_sink(ctx, "insertAdjacentHTML", argv[1]);
    return JS_UNDEFINED;
}
/* el.innerHTML = / el.outerHTML = tainted -> XSS sink (the #1 DOM XSS). */
static JSValue js_el_set_html(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic) {
    if (host_opaque(val)) emit_sink(ctx, magic ? "outerHTML" : "innerHTML", val);
    return JS_UNDEFINED;
}
static JSValue js_el_get_html(JSContext *ctx, JSValueConst this_val, int magic) { return JS_DupValue(ctx, g_opaque); }
static JSValue js_el_appendChild(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    lxb_dom_element_t *parent = JS_GetOpaque(this_val, g_el_class_id);
    lxb_dom_element_t *child = (argc > 0) ? JS_GetOpaque(argv[0], g_el_class_id) : NULL;
    if (parent && child) {
        lxb_dom_node_insert_child(lxb_dom_interface_node(parent), lxb_dom_interface_node(child));
        dom_insert_capture(lxb_dom_interface_node(child));
        script_maybe_load(child);   /* injected <script src> (computed URL) -> discovered as a chunk */
    }
    return (argc > 0) ? JS_DupValue(ctx, argv[0]) : JS_UNDEFINED;
}
/* reflected URL/identity properties (el.src = computedUrl / el.href / el.id) map to Lexbor attributes,
   so a bundle setting .src via PROPERTY (the common script-injection form) is captured + intercepted. */
static const char *refl_name(int magic) {
    switch (magic) { case 0: return "src"; case 1: return "href"; case 2: return "action"; case 3: return "id"; default: return ""; }
}
static JSValue js_el_refl_get(JSContext *ctx, JSValueConst this_val, int magic) {
    lxb_dom_element_t *el = JS_GetOpaque(this_val, g_el_class_id); if (!el) return JS_UNDEFINED;
    const char *n = refl_name(magic); size_t vl = 0;
    const lxb_char_t *v = lxb_dom_element_get_attribute(el, (const lxb_char_t *)n, strlen(n), &vl);
    return v ? JS_NewStringLen(ctx, (const char *)v, vl) : JS_NewString(ctx, "");
}
static JSValue js_el_refl_set(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic) {
    lxb_dom_element_t *el = JS_GetOpaque(this_val, g_el_class_id); if (!el) return JS_UNDEFINED;
    const char *n = refl_name(magic);
    if ((magic == 1 || magic == 2) && host_opaque(val)) emit_sink(ctx, "href", val);   /* @S: tainted el.href/.action (javascript:/redirect) */
    const char *v = JS_ToCString(ctx, val);
    if (v) { dom_attr_capture(el, n); lxb_dom_element_set_attribute(el, (const lxb_char_t *)n, strlen(n), (const lxb_char_t *)v, strlen(v)); JS_FreeCString(ctx, v); }
    return JS_UNDEFINED;
}
static void el_install_methods(JSContext *ctx, JSValue proto) {
    JS_SetPropertyStr(ctx, proto, "getAttribute", JS_NewCFunction(ctx, js_el_getAttribute, "getAttribute", 1));
    JS_SetPropertyStr(ctx, proto, "setAttribute", JS_NewCFunction(ctx, js_el_setAttribute, "setAttribute", 2));
    JS_SetPropertyStr(ctx, proto, "appendChild", JS_NewCFunction(ctx, js_el_appendChild, "appendChild", 1));
    JS_SetPropertyStr(ctx, proto, "insertAdjacentHTML", JS_NewCFunction(ctx, js_el_insertAdjacentHTML, "insertAdjacentHTML", 2));
    JS_SetPropertyStr(ctx, proto, "querySelector", JS_NewCFunction(ctx, js_el_querySelector, "querySelector", 1));
    JS_SetPropertyStr(ctx, proto, "getTextContent", JS_NewCFunction(ctx, js_el_textContent, "getTextContent", 0));
    for (int i = 0; i < 2; i++) {   /* innerHTML (magic 0) / outerHTML (magic 1) setter = XSS sink */
        JSAtom a = JS_NewAtom(ctx, i ? "outerHTML" : "innerHTML");
        JS_DefinePropertyGetSet(ctx, proto, a,
            JS_NewCFunctionMagic(ctx, (JSCFunctionMagic *)js_el_get_html, "get", 0, JS_CFUNC_getter_magic, i),
            JS_NewCFunctionMagic(ctx, (JSCFunctionMagic *)js_el_set_html, "set", 1, JS_CFUNC_setter_magic, i),
            JS_PROP_CONFIGURABLE);
        JS_FreeAtom(ctx, a);
    }
    static const char *refl[] = { "src", "href", "action", "id" };
    for (int i = 0; i < 4; i++) {
        JSAtom a = JS_NewAtom(ctx, refl[i]);
        JS_DefinePropertyGetSet(ctx, proto, a,
            JS_NewCFunctionMagic(ctx, (JSCFunctionMagic *)js_el_refl_get, "get", 0, JS_CFUNC_getter_magic, i),
            JS_NewCFunctionMagic(ctx, (JSCFunctionMagic *)js_el_refl_set, "set", 1, JS_CFUNC_setter_magic, i),
            JS_PROP_CONFIGURABLE);
        JS_FreeAtom(ctx, a);
    }
}
static JSValue js_doc_createElement(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (!g_dom || argc < 1) return JS_NULL;
    const char *tag = JS_ToCString(ctx, argv[0]); if (!tag) return JS_NULL;
    lxb_dom_element_t *el = lxb_dom_document_create_element(lxb_dom_interface_document(g_dom), (const lxb_char_t *)tag, strlen(tag), NULL);
    JS_FreeCString(ctx, tag);
    return el_wrap(ctx, el);
}
/* document.write(tainted) -> XSS sink. */
static JSValue js_doc_write(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    for (int i = 0; i < argc; i++) if (host_opaque(argv[i])) { emit_sink(ctx, "document.write", argv[i]); break; }
    return JS_UNDEFINED;
}
/* eval(tainted) -> code-injection sink; eval(concrete) -> forced-execute (dynamic code path, orphans). */
static JSValue js_eval(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (argc < 1) return JS_UNDEFINED;
    if (host_opaque(argv[0])) { emit_sink(ctx, "eval", argv[0]); return JS_DupValue(ctx, g_opaque); }
    if (JS_IsString(argv[0])) {
        size_t len = 0; const char *s = JS_ToCStringLen(ctx, &len, argv[0]);
        JSValue r = s ? JS_Eval(ctx, s, len, "<eval>", JS_EVAL_TYPE_GLOBAL) : JS_UNDEFINED;
        if (s) JS_FreeCString(ctx, s);
        return r;
    }
    return JS_DupValue(ctx, argv[0]);   /* non-string: spec returns the arg unchanged */
}
static JSValue js_doc_getElementById(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (argc < 1) return JS_NULL;
    const char *id = JS_ToCString(ctx, argv[0]); if (!id) return JS_NULL;
    char sel[512]; snprintf(sel, sizeof sel, "#%s", id);
    lxb_dom_element_t *el = dom_select_first(sel, strlen(sel));
    JS_FreeCString(ctx, id);
    return el_wrap(ctx, el);
}
static JSValue js_doc_querySelector(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (argc < 1) return JS_NULL;
    const char *s = JS_ToCString(ctx, argv[0]); if (!s) return JS_NULL;
    lxb_dom_element_t *el = dom_select_first(s, strlen(s));
    JS_FreeCString(ctx, s);
    return el_wrap(ctx, el);
}
/* Parse page HTML into the live DOM + init the CSS-selector engine. Returns 0 on success. */
static int dom_init(const char *html, size_t len) {
    g_dom = lxb_html_document_create();
    if (!g_dom) return -1;
    if (html && len && lxb_html_document_parse(g_dom, (const lxb_char_t *)html, len) != LXB_STATUS_OK) return -1;
    return 0;
}

/* Run the document's own scripts (in document order) against the real DOM — the moat runs the page's
   UNMODIFIED bundle, so the ENGINE extracts + executes them, not a bridge-side scrape. Inline <script>
   is eval'd in the global scope; external <script src> is surfaced as @CHUNK for safe-fetch + forced-
   execute (step C — computed/injected src is the same edge). */
struct scr_ctx { lxb_dom_element_t **els; int n, cap; };
static lxb_status_t scr_collect_cb(lxb_dom_node_t *node, lxb_css_selector_specificity_t s, void *vp) {
    struct scr_ctx *c = vp; (void)s;
    if (c->n >= c->cap) { int nc = c->cap ? c->cap * 2 : 16;
        lxb_dom_element_t **ne = realloc(c->els, (size_t)nc * sizeof(*ne)); if (!ne) return LXB_STATUS_OK; c->els = ne; c->cap = nc; }
    c->els[c->n++] = lxb_dom_interface_element(node);
    return LXB_STATUS_OK;   /* collect all in document order; eval AFTER traversal (eval may mutate the DOM) */
}
static void dom_run_scripts(JSContext *ctx) {
    if (!g_dom) return;
    lxb_css_parser_t *p = lxb_css_parser_create();
    if (!p || lxb_css_parser_init(p, NULL) != LXB_STATUS_OK) { if (p) lxb_css_parser_destroy(p, true); return; }
    lxb_css_selector_list_t *list = lxb_css_selectors_parse(p, (const lxb_char_t *)"script", 6);
    if (!list) { lxb_css_parser_destroy(p, true); return; }
    lxb_selectors_t *sel = lxb_selectors_create();
    if (!sel || lxb_selectors_init(sel) != LXB_STATUS_OK) { if (sel) lxb_selectors_destroy(sel, true); lxb_css_parser_destroy(p, true); return; }
    struct scr_ctx c = { NULL, 0, 0 };
    lxb_selectors_find(sel, lxb_dom_interface_node(g_dom), list, scr_collect_cb, &c);
    lxb_selectors_destroy(sel, true); lxb_css_parser_destroy(p, true);
    uint32_t bh = 2166136261u;   /* FNV-1a bundle identity over the page's OWN scripts (Lexbor DOM, not regex) */
    for (int i = 0; i < c.n; i++) {
        lxb_dom_element_t *el = c.els[i];
        size_t sl = 0;
        const lxb_char_t *src = lxb_dom_element_get_attribute(el, (const lxb_char_t *)"src", 3, &sl);
        if (src && sl) {
            char *cu = strndup((const char *)src, sl);
            if (cu) { arr_push_str(g_ctx, g_chunkurls, cu); free(cu); }             /* external src -> @RESULT.chunkUrls */
            for (size_t k = 0; k < sl; k++) { bh ^= src[k]; bh *= 16777619u; }     /* external src URL -> bundle id */
            bh ^= '|'; bh *= 16777619u;
            continue;
        }
        size_t tl = 0;
        lxb_char_t *txt = lxb_dom_node_text_content(lxb_dom_interface_node(el), &tl);
        if (txt && tl) {
            for (size_t k = 0; k < tl; k++) { bh ^= txt[k]; bh *= 16777619u; }     /* inline body -> bundle id */
            bh ^= '|'; bh *= 16777619u;
            JSValue v = JS_Eval(ctx, (const char *)txt, tl, "<script>", JS_EVAL_TYPE_GLOBAL);
            if (JS_IsException(v)) js_std_dump_error(ctx);
            JS_FreeValue(ctx, v);
        }
        if (txt) lxb_dom_document_destroy_text(lxb_dom_interface_node(el)->owner_document, txt);
    }
    g_bundle_id = bh ? bh : 1;
    free(c.els);
}

/* __fork(fn, hint?): add a flow to the ONE registry (a STARTER, fresh decision vector). */
static JSValue js_fork(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    if (argc < 1 || !JS_IsFunction(ctx, argv[0])) return JS_ThrowTypeError(ctx, "__fork(fn)");
    double hint = 0; if (argc > 1) JS_ToFloat64(ctx, &hint, argv[1]);
    reg_add(ctx, JS_DupValue(ctx, argv[0]), hint, 0, NULL, 0);
    return JS_UNDEFINED;
}

/* __yield(): PARK the running flow; its resolve fn becomes a RESUMER carrying the flow's value. */
static JSValue js_yield(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    JSValue rf[2]; JSValue promise = JS_NewPromiseCapability(ctx, rf);
    if (JS_IsException(promise)) return promise;
    reg_add(ctx, rf[0], g_cur_val, 1, NULL, 0);
    JS_FreeValue(ctx, rf[1]);
    return promise;
}

/* orphan flow source — CONTINUOUS discovery, NOT a one-shot phase. Called every scheduler iteration so
   functions defined DYNAMICALLY during a forced flow (a login-gated lazy CHUNK: eval/import of fetched
   JS reached only by forcing the auth branch) become orphan flows and get driven -> we learn the
   logged-in surface while logged out. Already-executed fns are not returned by JS_CollectOrphans;
   already-queued fns are dup-skipped. Idempotent: re-running adds only NEW never-executed functions. */
static int seed_orphans(JSContext *ctx)
{
    static JSValue buf[4096];
    int n = JS_CollectOrphans(ctx, buf, 4096), seeded = 0;
    for (int i = 0; i < n; i++) {
        int dup = 0;
        for (int j = 0; j < g_reg_n; j++)
            if (!g_reg[j].is_resume && JS_VALUE_GET_PTR(g_reg[j].handle) == JS_VALUE_GET_PTR(buf[i])) { dup = 1; break; }
        /* also skip one already recorded in the stable buffer (queued/parked, not yet run) */
        if (!dup) for (int j = 0; j < g_orphan_n; j++)
            if (JS_VALUE_GET_PTR(g_orphan_buf[j]) == JS_VALUE_GET_PTR(buf[i])) { dup = 1; break; }
        if (dup) { JS_FreeValue(ctx, buf[i]); continue; }
        int idx = -1;
        if (g_orphan_n < 4096) { idx = g_orphan_n; g_orphan_buf[g_orphan_n++] = JS_DupValue(ctx, buf[i]); }  /* buffer owns a ref (stable locator) */
        if (g_resume_mode) { JS_FreeValue(ctx, buf[i]); continue; }   /* resume: build locators only; recipes are seeded explicitly */
        reg_add(ctx, buf[i], 1.0, 0, NULL, 0);
        g_reg[g_reg_n - 1].orphan_idx = idx;
        seeded++;
    }
    return seeded;   /* count surfaces in @RESULT._orphans (via g_orphan_n) — no dead @ORPHANS line */
}
static JSValue js_drive_orphans(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{ return JS_NewInt32(ctx, seed_orphans(ctx)); }

/* Per-flow isolation is the engine COW (JS_CowSetActive/JS_CowRevert): shared-state writes (var-refs =
   globals/lexicals/closures; baseline-object property mutations) are captured while a flow explores, and
   reverted to the post-boot BASELINE before the next STARTER runs — so an independent flow never sees
   another's writes, yet a flow sees its OWN writes within its run. */

/* WFQ order key (ORDER-only, never drops a work item): base + accumulated value-of-information
   (emits raise val) + an explore/fairness floor for rarely-scheduled flows - a CPU-since-emit decay
   so a monopolizer (burns CPU, emits nothing) SINKS below productive/unrun flows and gets starved.
   This is the ONE policy the host-level priority.js uses too; same formula, two levels. */
static double flow_weight(const Flow *f)
{
    return 1.0 + f->val + 0.5 / (double)(f->visits + 1) - 0.01 * f->cpu;
}

/* The value-driven yield decision, called by the engine at each loop back-edge of the running TOP
   flow frame. Ticks CPU (accounting, NOT a cap — the flow is resumable, never truncated), then yields
   iff a PARKED flow now outranks the running one. The empty-delta guard keeps interleaving correct
   without per-flow deltas yet: a shared-state writer runs to completion (never suspended mid-write).
   SHARED STATE IS BOTH VIEWS — the JS heap (COW log, JS_CowDepth) AND the Lexbor DOM (undo log,
   g_dom_undo_n). A flow mid-DOM-mutation must NOT be suspended: dom_revert reverts the GLOBAL undo log
   to baseline before the next starter, so a DOM writer preempted mid-write would resume against a DOM
   its own writes were erased from. Guard on BOTH so "resumable DOM state switching" is sound (a DOM
   writer runs to completion, same as a JS-heap writer); per-flow DOM deltas are the later refinement
   that would let even a DOM writer be preempted. */
static int wfq_yield(void)
{
    if (!g_cur_flow) return 0;
    g_cur_flow->cpu += 1.0;
    if (JS_CowDepth() > 0 || g_dom_undo_n > 0) return 0;   /* don't preempt a flow mid shared-state write (JS heap OR DOM) */
    double rw = flow_weight(g_cur_flow);
    for (int i = 0; i < g_reg_n; i++)
        if (flow_weight(&g_reg[i]) > rw) return 1;   /* a parked flow now outranks the running quantum */
    return 0;
}

/* Emit the remaining frontier as compact REPLAY recipes (orphan_idx + decision-vector) and clear it.
   A parked flow is reconstructed next session by re-running boot + replaying its decisions — never by
   serializing a live continuation. The host persists these lines to IDB; @PARK/@PARKED are read back. */
static void park_frontier(JSContext *ctx)
{
    int parked = 0;
    for (int i = 0; i < g_reg_n; i++) {
        Flow *f = &g_reg[i];
        if (f->orphan_idx >= 0 && f->orphan_idx < g_orphan_n) {   /* orphan-derived flows are replay-locatable */
            uint32_t oh = JS_OrphanHash(ctx, g_orphan_buf[f->orphan_idx]);   /* stable identity, not the positional index */
            if (oh) {
                char rec[80]; int o = snprintf(rec, sizeof rec, "%u,", oh);
                for (int j = 0; j < f->dec_n && o < (int)sizeof(rec) - 1; j++) rec[o++] = f->dec[j] ? '1' : '0';
                rec[o] = 0;
                arr_push_str(ctx, g_park, rec);   /* "hash,decbits" replay recipe -> @RESULT._park */
                parked++;
            }
        }
        if (f->fs) JS_FlowFree(g_rt, f->fs);
        JS_FreeValue(ctx, f->handle);
        free(f->dec);
    }
    g_reg_n = 0;
    (void)parked;   /* recipes accumulated into g_park -> @RESULT._park (no separate @PARKED line) */
}

/* The ONE scheduler loop: pick the highest-WEIGHT flow (NON-FIFO), run it as a preemptible heap-frame
   quantum, re-queue it if it suspended (interleave), repeat. BFS by value-of-information: shallow
   high-emit flows finish ahead of the deep residue, which is starved to ~0 CPU (resumable). */
static void scheduler_run(JSContext *ctx)
{
    for (;;) {
        seed_orphans(ctx);          /* CONTINUOUS: pick up functions a prior flow defined dynamically (chunks) */
        if (g_reg_n == 0) break;
        /* HOST QUANTUM (cross-page fairness) = RESOURCE PRESSURE on ALL work, not just starters: after
           g_quantum flow DISPATCHES (starter OR resume/fetch-round), PARK the rest of the frontier as compact
           replay recipes (orphan_idx + decision-vector) and stop — resumable next burst/session. Counting
           resumes too is what makes a single DEEP await/fetch-loop flow yield to park (it never adds starters),
           so no step-cap crutch is needed. Not a bound: parked flows re-drive by re-running boot + decisions. */
        if (g_quantum > 0 && g_work >= g_quantum) { park_frontier(ctx); break; }
        int best = 0;
        for (int i = 1; i < g_reg_n; i++)
            if (flow_weight(&g_reg[i]) > flow_weight(&g_reg[best])) best = i;
        Flow f = g_reg[best];                       /* f is a STABLE COPY: reg_add during the run may realloc g_reg */
        g_reg_n--; g_reg[best] = g_reg[g_reg_n];    /* swap-remove */
        f.visits++;
        g_work++;                                   /* one flow got CPU (starter OR resume) — counts toward the quantum */
        g_cur_orphan_idx = f.orphan_idx;            /* siblings forked during this flow inherit its locator */
        g_running = 1; g_cur_val = f.val; g_cur_flow = &f;

        if (f.is_resume) {
            /* async __yield resume: drive the microtask to completion (a resolve fn, not sync-preemptible) */
            g_cur_fn = JS_UNDEFINED; g_dec_n = 0; g_c = 0;
            JS_SetFlowYieldHook(NULL);
            JSValue u = JS_UNDEFINED;
            JSValue r = JS_Call(ctx, f.handle, JS_UNDEFINED, 1, &u);
            if (JS_IsException(r)) js_std_dump_error(ctx);
            JS_FreeValue(ctx, r);
            JSContext *c1; int jr;
            while ((jr = JS_ExecutePendingJob(g_rt, &c1)) > 0) { }
            if (jr < 0) js_std_dump_error(c1 ? c1 : ctx);
            g_running = 0; g_cur_flow = NULL;
            JS_FreeValue(ctx, f.handle); free(f.dec);
            continue;
        }

        /* SYNC flow: load its per-flow scheduler state (decision vector + branch cursor). __branch
           consumes/extends g_dec + forks siblings; force-invoke with OPAQUE this+args (external input
           the tool must not concretely decide) so gates fork and computed URLs are shaped. */
        g_cur_fn = f.handle;
        g_dec_n = f.dec_n; g_dec_ensure(g_dec_n);
        for (int i = 0; i < g_dec_n; i++) g_dec[i] = f.dec ? f.dec[i] : 0;
        g_c = f.saved_c;

        if (f.fs == NULL) {                         /* STARTER: baseline + fresh heap frame */
            JS_CowRevert(ctx);                      /* revert JS shared-state to the post-boot baseline (safe: only empty-delta flows suspend) */
            dom_revert();                           /* revert DOM mutations to the post-boot baseline (per-flow isolation) */
            JSValue oargs[8]; for (int i = 0; i < 8; i++) oargs[i] = g_opaque;
            /* A 'message' listener's first arg is a MessageEvent whose .data is attacker-controlled
               (postMessage): drive it with the {pm} source-tagged event so a sink reaching e.data reports
               {pm} and the PoC assembler builds a postMessage-delivered PoC. */
            if (!JS_IsUndefined(g_msg_event) && is_msg_handler(f.handle)) oargs[0] = g_msg_event;
            /* Drive an orphan METHOD with its REAL receiver instance if one exists (this.field -> concrete
               boot value, a real example) else opaque this. Args stay opaque (external input). */
            JSValue recv = JS_FindReceiver(ctx, f.handle);
            JSValue this_val = JS_IsUndefined(recv) ? g_opaque : recv;
            f.fs = JS_FlowNew(ctx, f.handle, this_val, 8, oargs);
            JS_FreeValue(ctx, recv);
            if (!f.fs) {
                /* ASYNC/generator (or non-bytecode) orphan: not sync-preemptible — it self-suspends via
                   await/yield. Run via a plain call + drain its microtask chain to completion (the awaits
                   land emits). Preemption of these is the async-per-flow-delta task, not this path. */
                JS_SetFlowYieldHook(NULL);
                JSValue r = JS_Call(ctx, f.handle, g_opaque, 8, oargs);
                if (JS_IsException(r)) js_std_dump_error(ctx);
                JS_FreeValue(ctx, r);
                JSContext *c2; int jr2;
                while ((jr2 = JS_ExecutePendingJob(g_rt, &c2)) > 0) { }
                if (jr2 < 0) js_std_dump_error(c2 ? c2 : ctx);
                g_running = 0; g_cur_fn = JS_UNDEFINED; g_cur_flow = NULL;
                JS_FreeValue(ctx, f.handle); free(f.dec);
                continue;
            }
        }
        /* else SUSPENDED: resume in place. Its COW delta was empty at suspend + starters revert to
           baseline, so shared state == baseline == what this flow expects (it wrote nothing shared). */

        JS_SetFlowYieldHook(wfq_yield);
        JSValue out = JS_UNDEFINED;
        int st = JS_FlowResume(ctx, f.fs, &out);
        JS_SetFlowYieldHook(NULL);
        JSContext *c1; int jr;
        while ((jr = JS_ExecutePendingJob(g_rt, &c1)) > 0) { }
        if (jr < 0) js_std_dump_error(c1 ? c1 : ctx);
        g_running = 0; g_cur_fn = JS_UNDEFINED;

        if (st == 1) {
            /* SUSPENDED at a back-edge: snapshot per-flow scheduler state and RE-QUEUE (interleave). */
            f.saved_c = g_c;
            f.val = g_cur_val;
            if (g_dec_n > f.dec_n) {                /* grew (new branch decisions taken this quantum) */
                signed char *nd = (signed char *)malloc((size_t)(g_dec_n > 0 ? g_dec_n : 1));
                if (nd) { for (int i = 0; i < g_dec_n; i++) nd[i] = g_dec[i]; free(f.dec); f.dec = nd; }
            } else {
                for (int i = 0; i < g_dec_n && f.dec; i++) f.dec[i] = g_dec[i];
            }
            f.dec_n = g_dec_n;
            g_cur_flow = NULL;
            reg_readd(ctx, f);
        } else {
            /* COMPLETED/error: JS_FlowResume already freed the heap frame + its values. */
            if (JS_IsException(out)) js_std_dump_error(ctx);
            JS_FreeValue(ctx, out);
            g_cur_flow = NULL;
            JS_FreeValue(ctx, f.handle); free(f.dec);
        }
    }
    JS_SetFlowYieldHook(NULL);
}

/* ── Persistent-instance protocol ─────────────────────────────────────────────────
   ONE wasm instance per page, driven in steps by the offscreen: qjs_init (build runtime + env + boot +
   seed the frontier), qjs_step (advance the ONE scheduler), qjs_teardown. This replaces the old
   re-instantiate-and-re-run-per-pass model so chunks/fromReply/frontier become ONE continuous run
   (in-place suspend/resume) instead of re-running the whole page. */
static JSContext *g_ctx = NULL;
static int g_rc = 0;

KEEP int qjs_init(const char *boot, const char *html, const char *origin,
                  const char *replies, int quantum, const char *recipes)
{
    JSRuntime *rt = JS_NewRuntime();
    if (!rt) { fprintf(stderr, "@E {\"phase\":\"newruntime\"}\n"); return 1; }
    g_rt = rt;
    JS_SetMaxStackSize(rt, 4 * 1024 * 1024);
    JS_UpdateStackTop(rt);
    js_std_init_handlers(rt);
    JSContext *ctx = JS_NewContext(rt);
    if (!ctx) { fprintf(stderr, "@E {\"phase\":\"newcontext\"}\n"); JS_FreeRuntime(rt); return 1; }
    g_ctx = ctx;
    /* Reset all scheduler/frontier state so ONE wasm instance can serve many page analyses (init/run/
       teardown reused) with no cross-page bleed. The arrays themselves are reused (not re-malloc'd). */
    g_reg_n = 0; g_work = 0; g_emit_total = 0; g_running = 0; g_cur_flow = NULL; g_msg_handler_n = 0;
    g_cur_orphan_idx = -1; g_dec_n = 0; g_c = 0; g_resume_mode = 0; g_quantum = 0;
    g_pending_n = 0; g_chunk_n = 0; g_orphan_n = 0; g_dom_capture = 0;
    /* ENDPOINT/@S/etc. accumulators + the in-engine dedup fn — the engine builds the whole structured
       result and emits ONE @RESULT json at finalize (the host JSON.parses it; no host-side parse/identity). */
    g_endpoints = JS_NewArray(ctx); g_sinks = JS_NewArray(ctx); g_chunkurls = JS_NewArray(ctx);
    g_whys = JS_NewArray(ctx); g_park = JS_NewArray(ctx);
    g_dedup_fn = JS_Eval(ctx, DEDUP_JS, strlen(DEDUP_JS), "<dedup>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(g_dedup_fn)) { js_std_dump_error(ctx); g_dedup_fn = JS_UNDEFINED; }
    js_std_add_helpers(ctx, 0, NULL);
    js_init_module_std(ctx, "std");
    js_init_module_os(ctx, "os");

    if (origin && origin[0]) set_origin(origin);   /* real page principal (location.origin/host) */
    if (replies && replies[0]) {                   /* fromReply table: { url -> concrete reply body } */
        JSValue t = JS_ParseJSON(ctx, replies, strlen(replies), "<replies>");
        if (JS_IsException(t)) { JSValue e = JS_GetException(ctx); JS_FreeValue(ctx, e); }
        else g_reply_table = t;
    }
    g_quantum = quantum;                           /* host per-page CPU slice (0 = run to completion) */
    (void)recipes;                                 /* recipes are seeded in phase-2 qjs_begin(), after the host reads the bundle-id */

    JSValue g = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, g, "__emit", JS_NewCFunction(ctx, js_emit, "__emit", 1));
    JS_SetPropertyStr(ctx, g, "__fork", JS_NewCFunction(ctx, js_fork, "__fork", 2));
    JS_SetPropertyStr(ctx, g, "__yield", JS_NewCFunction(ctx, js_yield, "__yield", 0));
    JS_SetPropertyStr(ctx, g, "__branch", JS_NewCFunction(ctx, js_branch, "__branch", 0));
    JS_SetPropertyStr(ctx, g, "__opaque", JS_NewCFunction(ctx, js_opaque, "__opaque", 0));
    JS_SetPropertyStr(ctx, g, "fetch", JS_NewCFunction(ctx, js_fetch, "fetch", 2));
    JS_SetPropertyStr(ctx, g, "eval", JS_NewCFunction(ctx, js_eval, "eval", 1));   /* eval(tainted) -> @S; eval(concrete) -> forced-execute */
    /* Register the OPAQUE sentinel + the branch hook: a branch whose condition IS this object forks both
       arms via the decision-vector logic (real bundles: external input reaches OP_if as opaque). */
    JS_InitOpaqueClass(ctx);             /* register the shape-carrying opaque class */
    g_opaque = JS_NewOpaqueShaped(ctx, "{}");   /* the default opaque (shape "{}"); generic propagation dups it */
    JS_SetOpaqueMarker(g_opaque);
    JS_SetBranchHook(branch_decide);
    JS_SetDynImportHook(host_dyn_import);   /* dynamic import() -> force-fetch the ESM chunk in place */
    /* synthetic MessageEvent for driving 'message' handlers: .data is the {pm} source-tagged opaque. */
    g_msg_event = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, g_msg_event, "data", JS_NewOpaqueShaped(ctx, "{pm}"));
    JS_SetPropertyStr(ctx, g_msg_event, "origin", JS_DupValue(ctx, g_opaque));
    JS_SetPropertyStr(ctx, g_msg_event, "source", JS_DupValue(ctx, g_opaque));
    JS_SetPropertyStr(ctx, g_msg_event, "ports", JS_DupValue(ctx, g_opaque));
    JS_SetPropertyStr(ctx, g, "__driveOrphans", JS_NewCFunction(ctx, js_drive_orphans, "__driveOrphans", 0));

    /* Minimal browser environment: window/self = globalThis; OPAQUE external-input sources (location,
       document.cookie, navigator, referrer) so a real bundle's gate on external input auto-forks without
       synthetic args; addEventListener = no-op (the handler is then a never-fired orphan, driven). This
       is the seam the real Lexbor DOM + real safe-fetch plug into during the host rewire. */
    /* Real DOM: parse the page HTML (argv[2], optional) into a live Lexbor document + register the
       Element JS class. The page's own structure/config is CONCRETE; document.* reads it. */
    {
        if (dom_init(html, html ? strlen(html) : 0) != 0)
            fprintf(stderr, "@E {\"phase\":\"dom_init\"}\n");
        JS_NewClassID(rt, &g_el_class_id);
        JSClassDef el_def = { "Element" };   /* lexbor owns the nodes; no JS finalizer */
        JS_NewClass(rt, g_el_class_id, &el_def);
        JSValue el_proto = JS_NewObject(ctx);
        el_install_methods(ctx, el_proto);
        JS_SetClassProto(ctx, g_el_class_id, el_proto);
    }

    g_handlers = JS_NewArray(ctx);
    JS_SetPropertyStr(ctx, g, "__handlers", JS_DupValue(ctx, g_handlers));   /* reachable so handlers survive to orphan-collect */
    JS_SetPropertyStr(ctx, g, "window", JS_DupValue(ctx, g));
    JS_SetPropertyStr(ctx, g, "self",   JS_DupValue(ctx, g));
    JS_SetPropertyStr(ctx, g, "globalThis", JS_DupValue(ctx, g));
    JS_SetPropertyStr(ctx, g, "location", make_location(ctx));   /* concrete principal identity; search/hash opaque */
    JS_SetPropertyStr(ctx, g, "navigator", JS_DupValue(ctx, g_opaque));
    JS_SetPropertyStr(ctx, g, "addEventListener", JS_NewCFunction(ctx, js_add_listener, "addEventListener", 2));
    JS_SetPropertyStr(ctx, g, "removeEventListener", JS_NewCFunction(ctx, js_noop, "removeEventListener", 2));
    {
        JSValue doc = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, doc, "cookie", JS_DupValue(ctx, g_opaque));      /* external/auth input: opaque */
        JS_SetPropertyStr(ctx, doc, "referrer", JS_DupValue(ctx, g_opaque));    /* external input: opaque */
        JS_SetPropertyStr(ctx, doc, "URL", JS_NewString(ctx, g_origin));        /* page identity: CONCRETE for URL building */
        JS_SetPropertyStr(ctx, doc, "domain", JS_NewString(ctx, g_host));       /* page identity: CONCRETE */
        JS_SetPropertyStr(ctx, doc, "addEventListener", JS_NewCFunction(ctx, js_add_listener, "addEventListener", 2));
        JS_SetPropertyStr(ctx, doc, "querySelector", JS_NewCFunction(ctx, js_doc_querySelector, "querySelector", 1));   /* real Lexbor DOM */
        JS_SetPropertyStr(ctx, doc, "getElementById", JS_NewCFunction(ctx, js_doc_getElementById, "getElementById", 1));
        JS_SetPropertyStr(ctx, doc, "createElement", JS_NewCFunction(ctx, js_doc_createElement, "createElement", 1));   /* real element; appendChild intercepts <script src> */
        JS_SetPropertyStr(ctx, doc, "write", JS_NewCFunction(ctx, js_doc_write, "write", 1));       /* document.write(tainted) -> @S */
        JS_SetPropertyStr(ctx, doc, "writeln", JS_NewCFunction(ctx, js_doc_write, "writeln", 1));
        JS_SetPropertyStr(ctx, doc, "head", el_wrap(ctx, g_dom ? lxb_dom_interface_element(lxb_html_document_head_element(g_dom)) : NULL));
        JS_SetPropertyStr(ctx, doc, "body", el_wrap(ctx, g_dom ? lxb_dom_interface_element(lxb_html_document_body_element(g_dom)) : NULL));
        JS_SetPropertyStr(ctx, g, "document", doc);
    }
    /* Time/random are EXTERNAL INPUT -> OPAQUE: a branch on Math.random()/Date.now() must FORK both arms
       (not take a random one), and their VALUES are shapes not fabricated concretes. This is also a REPLAY
       SOUNDNESS requirement -- non-deterministic values would shift orphan-collection order between
       sessions and make a parked flow's (orphan_idx) recipe reconstruct the wrong flow. */
    {
        JSValue mo = JS_GetPropertyStr(ctx, g, "Math");
        if (JS_IsObject(mo)) JS_SetPropertyStr(ctx, mo, "random", JS_NewCFunction(ctx, js_opaque, "random", 0));
        JS_FreeValue(ctx, mo);
        JSValue dt = JS_GetPropertyStr(ctx, g, "Date");
        if (JS_IsObject(dt)) {
            JS_SetPropertyStr(ctx, dt, "now", JS_NewCFunction(ctx, js_opaque, "now", 0));
            JSValue dp = JS_GetPropertyStr(ctx, dt, "prototype");   /* new Date().getTime()/valueOf() -> opaque too */
            if (JS_IsObject(dp)) {
                JS_SetPropertyStr(ctx, dp, "getTime", JS_NewCFunction(ctx, js_opaque, "getTime", 0));
                JS_SetPropertyStr(ctx, dp, "valueOf", JS_NewCFunction(ctx, js_opaque, "valueOf", 0));
            }
            JS_FreeValue(ctx, dp);
        }
        JS_FreeValue(ctx, dt);
        JSValue perf = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, perf, "now", JS_NewCFunction(ctx, js_opaque, "now", 0));
        JS_SetPropertyStr(ctx, g, "performance", perf);
        /* Web Crypto: a MISSING host edge (crypto was undefined -> ReferenceError killed EVERY bundle that
           mints a token/UUID/id). randomUUID = external randomness -> OPAQUE (forks branches, shapes URLs,
           replay-sound); getRandomValues fills a numeric array in place (see stub); subtle = opaque (its
           digest/encrypt results are external-derived). */
        JSValue cr = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, cr, "randomUUID", JS_NewCFunction(ctx, js_opaque, "randomUUID", 0));
        JS_SetPropertyStr(ctx, cr, "getRandomValues", JS_NewCFunction(ctx, js_crypto_getrandom, "getRandomValues", 1));
        JS_SetPropertyStr(ctx, cr, "subtle", JS_DupValue(ctx, g_opaque));
        JS_SetPropertyStr(ctx, g, "crypto", cr);
        /* Timers: a deferred callback is a FLOW in the one scheduler (see js_set_timer), not a real wait.
           Missing these made every bundle that defers init in setTimeout learn nothing. */
        JS_SetPropertyStr(ctx, g, "setTimeout", JS_NewCFunction(ctx, js_set_timer, "setTimeout", 2));
        JS_SetPropertyStr(ctx, g, "setInterval", JS_NewCFunction(ctx, js_set_timer, "setInterval", 2));
        JS_SetPropertyStr(ctx, g, "requestAnimationFrame", JS_NewCFunction(ctx, js_set_timer, "requestAnimationFrame", 1));
        JS_SetPropertyStr(ctx, g, "requestIdleCallback", JS_NewCFunction(ctx, js_set_timer, "requestIdleCallback", 1));
        JS_SetPropertyStr(ctx, g, "clearTimeout", JS_NewCFunction(ctx, js_noop, "clearTimeout", 1));
        JS_SetPropertyStr(ctx, g, "clearInterval", JS_NewCFunction(ctx, js_noop, "clearInterval", 1));
        JS_SetPropertyStr(ctx, g, "cancelAnimationFrame", JS_NewCFunction(ctx, js_noop, "cancelAnimationFrame", 1));
        JS_SetPropertyStr(ctx, g, "structuredClone", JS_NewCFunction(ctx, js_structured_clone, "structuredClone", 1));
        /* Web storage: values are external input -> opaque getItem; writes no-op. */
        for (int si = 0; si < 2; si++) {
            JSValue st = JS_NewObject(ctx);
            JS_SetPropertyStr(ctx, st, "getItem", JS_NewCFunction(ctx, js_storage_get, "getItem", 1));
            JS_SetPropertyStr(ctx, st, "setItem", JS_NewCFunction(ctx, js_noop, "setItem", 2));
            JS_SetPropertyStr(ctx, st, "removeItem", JS_NewCFunction(ctx, js_noop, "removeItem", 1));
            JS_SetPropertyStr(ctx, st, "clear", JS_NewCFunction(ctx, js_noop, "clear", 0));
            JS_SetPropertyStr(ctx, st, "key", JS_NewCFunction(ctx, js_storage_get, "key", 1));
            JS_SetPropertyStr(ctx, g, si ? "sessionStorage" : "localStorage", st);
        }
        /* URL / URLSearchParams: endpoint construction (see js_url_ctor). */
        JS_SetPropertyStr(ctx, g, "URL", JS_NewCFunction2(ctx, js_url_ctor, "URL", 2, JS_CFUNC_constructor, 0));
        JS_SetPropertyStr(ctx, g, "URLSearchParams", JS_NewCFunction2(ctx, js_searchparams_ctor, "URLSearchParams", 1, JS_CFUNC_constructor, 0));
        JS_SetPropertyStr(ctx, g, "Request", JS_NewCFunction2(ctx, js_request_ctor, "Request", 2, JS_CFUNC_constructor, 0));
        /* fetch-API + encoding + misc Web objects: opaque-read / no-op-write, so a bundle that constructs
           them doesn't ReferenceError and any value read out stays opaque. */
        const char *webctors[] = { "Headers", "Response", "FormData", "Blob", "File", "AbortController",
                                   "TextEncoder", "TextDecoder", "FileReader", "EventSource", "BroadcastChannel" };
        for (size_t wi = 0; wi < sizeof webctors / sizeof webctors[0]; wi++)
            JS_SetPropertyStr(ctx, g, webctors[wi], JS_NewCFunction2(ctx, js_webobj_ctor, webctors[wi], 1, JS_CFUNC_constructor, 0));
    }
    JS_FreeValue(ctx, g);

    if (boot) {
        JSValue v = JS_Eval(ctx, boot, strlen(boot), "<boot>", JS_EVAL_TYPE_GLOBAL);
        if (JS_IsException(v)) { js_std_dump_error(ctx); g_rc = 1; }
        JS_FreeValue(ctx, v);
        dom_run_scripts(ctx);   /* run the page's own inline scripts + compute g_bundle_id (Lexbor <script> scan) */
    }
    return 0;   /* boot done; g_bundle_id is now readable via qjs_bundle_id(). Host reads it, looks up the parked
                   frontier by ORIGIN|bundle-id in IDB (its only job), then calls qjs_begin(recipes) to seed. */
}

/* The document's stable bundle IDENTITY = FNV-1a over its OWN scripts, computed by the REAL Lexbor <script>
   scan in dom_run_scripts (never a host-side regex). The host uses origin|this as the frontier key. */
KEEP unsigned qjs_bundle_id(void) { return (unsigned)g_bundle_id; }

/* Phase 2 (after qjs_init + the host's frontierGet by bundle-id): seed the frontier and fix the COW baseline.
   recipes NULL/"" -> a fresh visit (drive all orphans); non-empty -> RESUME by re-creating each parked flow,
   located by its function SOURCE hash (JS_OrphanHash), decisions replayed by the scheduler. */
KEEP void qjs_begin(const char *recipes)
{
    JSContext *ctx = g_ctx;
    if (!ctx) return;
    g_resume_mode = (recipes && recipes[0]) ? 1 : 0;
    seed_orphans(ctx);      /* normal: seed fresh orphan flows. resume: build g_orphan_buf locators only. */
    if (g_resume_mode) {
        /* RESUME: each recipe "hash,dec" re-creates a flow by LOCATING the orphan whose stable SOURCE identity
           (JS_OrphanHash) matches — robust to collection order/context. A recipe whose function is absent in
           THIS context simply doesn't match (skipped) — never drives the wrong one. */
        const char *p = recipes; int resumed = 0;
        while (*p) {
            uint32_t want = (uint32_t)strtoul(p, NULL, 10);
            const char *comma = strchr(p, ','), *semi = strchr(p, ';');
            signed char *dec = NULL; int dec_n = 0;
            if (comma && (!semi || comma < semi)) {
                const char *d = comma + 1, *end = semi ? semi : d + strlen(d);
                dec_n = (int)(end - d);
                if (dec_n > 0) { dec = (signed char *)malloc((size_t)dec_n); for (int i = 0; i < dec_n; i++) dec[i] = (d[i] == '1') ? 1 : 0; }
            }
            int found = -1;
            for (int oi = 0; oi < g_orphan_n; oi++) { if (JS_OrphanHash(ctx, g_orphan_buf[oi]) == want) { found = oi; break; } }
            if (found >= 0) {
                reg_add(ctx, JS_DupValue(ctx, g_orphan_buf[found]), 1.0, 0, dec, dec_n);
                g_reg[g_reg_n - 1].orphan_idx = found; resumed++;
            } else free(dec);
            if (!semi) break; p = semi + 1;
        }
        printf("@RESUMED %d\n", resumed); fflush(stdout);
    }
    JS_CowSetActive(1);   /* baseline = post-boot state; capture shared-state writes during flow exploration */
    g_dom_capture = 1;    /* DOM baseline is now fixed too; capture flow DOM mutations for per-flow revert */
}

/* The distinct pending fetch urls (newline-joined) the offscreen must safe-fetch. Static buffer. */
KEEP const char *qjs_pending(void)
{
    static char *buf = NULL; static size_t cap = 0;
    size_t need = 1;
    for (int i = 0; i < g_pending_n; i++) if (g_pending[i].url) need += strlen(g_pending[i].url) + 1;
    if (need > cap) { char *n = realloc(buf, need); if (!n) return ""; buf = n; cap = need; }
    size_t off = 0;
    for (int i = 0; i < g_pending_n; i++) {
        if (!g_pending[i].url) continue;
        int dup = 0;
        for (int j = 0; j < i; j++) if (g_pending[j].url && strcmp(g_pending[j].url, g_pending[i].url) == 0) { dup = 1; break; }
        if (dup) continue;
        size_t l = strlen(g_pending[i].url);
        memcpy(buf + off, g_pending[i].url, l); off += l; buf[off++] = '\n';
    }
    buf[off] = 0;
    return buf;
}
/* Chunk urls to fetch (as JS) + eval in place (newline-joined). Static buffer. */
KEEP const char *qjs_chunks(void)
{
    static char *buf = NULL; static size_t cap = 0;
    size_t need = 1;
    for (int i = 0; i < g_chunk_n; i++) need += strlen(g_chunk_pending[i]) + 1;
    if (need > cap) { char *n = realloc(buf, need); if (!n) return ""; buf = n; cap = need; }
    size_t off = 0;
    for (int i = 0; i < g_chunk_n; i++) { size_t l = strlen(g_chunk_pending[i]); memcpy(buf + off, g_chunk_pending[i], l); off += l; buf[off++] = '\n'; }
    buf[off] = 0;
    return buf;
}
/* Resolve every pending consume of `url` with the concrete body (empty -> opaque); cache in the reply
   table so a later response of the same url is concrete too. If `url` is a pending CHUNK, eval its body
   in place — extending the page BASELINE (cow off during eval), so its defs are permanent (not a flow
   write to be reverted) and its orphans get driven on the next step. Continuations run on the next step. */
KEEP void qjs_provide(const char *url, const char *body)
{
    JSContext *ctx = g_ctx; if (!ctx || !url) return;
    for (int i = 0; i < g_chunk_n; i++) {
        if (strcmp(g_chunk_pending[i], url) != 0) continue;
        if (body && body[0]) {
            JS_CowRevert(ctx);                                   /* to baseline (parked flows have empty delta) */
            int dsv = g_dom_capture; g_dom_capture = 0; JS_CowSetActive(0);
            JSValue v = JS_Eval(ctx, body, strlen(body), url, JS_EVAL_TYPE_GLOBAL);   /* chunk is new page code */
            if (JS_IsException(v)) js_std_dump_error(ctx);
            JS_FreeValue(ctx, v);
            JS_CowSetActive(1); g_dom_capture = dsv;
        }
        free(g_chunk_pending[i]);
        for (int j = i; j < g_chunk_n - 1; j++) g_chunk_pending[j] = g_chunk_pending[j + 1];
        g_chunk_n--;
        return;
    }
    int has = body && body[0];
    if (has && JS_IsObject(g_reply_table)) JS_SetPropertyStr(ctx, g_reply_table, url, JS_NewString(ctx, body));
    for (int i = 0; i < g_pending_n; i++) {
        if (!g_pending[i].url || strcmp(g_pending[i].url, url) != 0) continue;
        JSValue v;
        if (has) { JSValue bs = JS_NewString(ctx, body); v = reply_value(ctx, bs, g_pending[i].is_json); JS_FreeValue(ctx, bs); }
        else v = JS_DupValue(ctx, g_opaque);
        JSValue r = JS_Call(ctx, g_pending[i].rf, JS_UNDEFINED, 1, (JSValueConst *)&v); JS_FreeValue(ctx, r); JS_FreeValue(ctx, v);
        JS_FreeValue(ctx, g_pending[i].rf); free(g_pending[i].url);
        g_pending[i].rf = JS_UNDEFINED; g_pending[i].url = NULL;
    }
    int w = 0; for (int i = 0; i < g_pending_n; i++) if (g_pending[i].url) g_pending[w++] = g_pending[i];
    g_pending_n = w;
}
/* Resolve ALL remaining pending with opaque (node CLI / finalize): chains continue as shapes. */
KEEP void qjs_finalize(void)
{
    JSContext *ctx = g_ctx; if (!ctx) return;
    for (int i = 0; i < g_pending_n; i++) {
        if (!g_pending[i].url) continue;
        JSValue v = JS_DupValue(ctx, g_opaque);
        JSValue r = JS_Call(ctx, g_pending[i].rf, JS_UNDEFINED, 1, (JSValueConst *)&v); JS_FreeValue(ctx, r); JS_FreeValue(ctx, v);
        JS_FreeValue(ctx, g_pending[i].rf); free(g_pending[i].url); g_pending[i].url = NULL;
    }
    g_pending_n = 0;
    for (int i = 0; i < g_chunk_n; i++) free(g_chunk_pending[i]);   /* node CLI can't fetch chunks -> drop */
    g_chunk_n = 0;
}
/* Advance the ONE scheduler; drain microtasks. Return 1 = NEED_FETCH (flows parked awaiting a real
   reply the offscreen must provide via qjs_provide), else 0 = DONE. */
KEEP int qjs_step(void)
{
    if (!g_ctx) return 0;
    scheduler_run(g_ctx);
    js_std_loop(g_ctx);
    return (g_pending_n > 0 || g_chunk_n > 0) ? 1 : 0;   /* NEED_FETCH: replies and/or chunks */
}

KEEP void qjs_teardown(void)
{
    JSContext *ctx = g_ctx;
    if (!ctx) return;
    qjs_finalize();   /* resolve any stragglers opaque so no promise leaks */
    emit_result(ctx);   /* dedup in-engine + emit the ONE @RESULT json (endpoints/sinks/chunks/errors/park/_emit) */
    /* Clean teardown (else JS_FreeRuntime asserts gc_obj_list non-empty): stop + revert the COW log so
       its held baseline values return to their slots, and drop the opaque marker. */
    JS_CowSetActive(0);
    JS_CowRevert(ctx);
    g_dom_capture = 0; dom_revert();   /* drop DOM undo log (restore baseline) before teardown */
    JS_SetOpaqueMarker(JS_UNDEFINED); JS_SetBranchHook(NULL);
    JS_FreeValue(ctx, g_opaque); g_opaque = JS_UNDEFINED;
    JS_FreeValue(ctx, g_reply_table); g_reply_table = JS_UNDEFINED;
    JS_FreeValue(ctx, g_endpoints); g_endpoints = JS_UNDEFINED;
    JS_FreeValue(ctx, g_sinks); g_sinks = JS_UNDEFINED;
    JS_FreeValue(ctx, g_chunkurls); g_chunkurls = JS_UNDEFINED;
    JS_FreeValue(ctx, g_whys); g_whys = JS_UNDEFINED;
    JS_FreeValue(ctx, g_park); g_park = JS_UNDEFINED;
    JS_FreeValue(ctx, g_dedup_fn); g_dedup_fn = JS_UNDEFINED;
    for (int i = 0; i < g_orphan_n; i++) JS_FreeValue(ctx, g_orphan_buf[i]);
    g_orphan_n = 0;
    JS_FreeValue(ctx, g_handlers); g_handlers = JS_UNDEFINED;
    JS_FreeValue(ctx, g_msg_event); g_msg_event = JS_UNDEFINED; g_msg_handler_n = 0;
    js_std_free_handlers(g_rt);
    JS_FreeContext(ctx);
    JS_FreeRuntime(g_rt);
    g_ctx = NULL; g_rt = NULL;
    fflush(stdout);
}

/* node CLI entry (design-narrowing only): drive the persistent protocol once. */
int main(int argc, char **argv)
{
    const char *boot    = (argc > 1) ? argv[1] : NULL;
    const char *html    = (argc > 2) ? argv[2] : NULL;
    const char *origin  = (argc > 3) ? argv[3] : NULL;
    const char *replies = (argc > 4) ? argv[4] : NULL;
    int         quantum = (argc > 5 && argv[5][0]) ? atoi(argv[5]) : 0;
    const char *recipes = (argc > 6) ? argv[6] : NULL;
    if (qjs_init(boot, html, origin, replies, quantum, recipes) != 0) return 1;
    qjs_begin(recipes);   /* phase 2: seed the frontier (fresh or resume) + fix the COW baseline */
    if (boot) { while (qjs_step() == 1) qjs_finalize(); }   /* node CLI has no network -> resolve pending opaque (shapes) */
    qjs_teardown();
    return g_rc;
}
