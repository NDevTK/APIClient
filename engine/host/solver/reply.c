/* Fetch Response + reply-body learning — see reply.h. */
#include "solver/reply.h"
#include "solver/concolic.h"   /* g_concolic, js_concolic_stub */
#include "platform/url.h"      /* has_hole */
#include "check.h"             /* CHECK — a dropped parked reply never resolves, losing the endpoint */
#include <string.h>
#include <stdlib.h>

/* Borrowed from main.c (the scheduler side): the reply-body table (host-seeded + written by the provision
   re-run), the resolved-promise helper, and the reply-fetch registrar (enqueues a bounded GET). */
extern JSValue g_reply_table;
extern JSValue js_resolved(JSContext *ctx, JSValue val);
extern void reply_fetch_register(const char *url, int is_json);

static JSValue js_resp_body(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{ return js_resolved(ctx, js_concolic(ctx, "{reply}", JS_UNDEFINED)); }
/* the concrete reply body injected onto this Response (fromReply), or JS_UNDEFINED */
static JSValue resp_body_str(JSContext *ctx, JSValueConst this_val) {
    JSValue b = JS_GetPropertyStr(ctx, this_val, "__body");
    if (JS_IsString(b)) return b;
    JS_FreeValue(ctx, b); return JS_UNDEFINED;
}
/* CONCOLIC leaf-wrap: a trusted loaded reply is ONE value whose STRUCTURE stays REAL (Object.keys/map/for-in/
   array index all work natively) but whose STRING/BOOL leaves become concolic — each FORKS at a branch (so a
   role/flag gate reaches the gated endpoint) AND carries its real value as the example (so a gate-INDEPENDENT
   field keeps its real value on the forced arm: /api/billing/enterprise/acme-42, not /{}). NUMBERS stay
   concrete — an opaque numeric would make `for(i<n)` / `.length` loops infinite (the known catastrophe).
   parse a concrete reply body into the value json()/text() should resolve to via the SHARED JS_ConcolicWrap
   (the ONE home, also used by JSON.parse of an SSR data blob — same trust boundary: loaded same-origin data). */
static JSValue reply_value(JSContext *ctx, JSValueConst body_str, int is_json) {
    if (!is_json) return JS_DupValue(ctx, body_str);   /* text: unchanged (avoid regressing text->JSON.parse) */
    size_t len = 0; const char *s = JS_ToCStringLen(ctx, &len, body_str);
    JSValue parsed = s ? JS_ParseJSON(ctx, s, len, "<reply>") : JS_EXCEPTION;
    if (s) JS_FreeCString(ctx, s);
    if (JS_IsException(parsed)) { JSValue e = JS_GetException(ctx); JS_FreeValue(ctx, e); return js_concolic(ctx, "{reply}", JS_UNDEFINED); }
    return JS_ConcolicWrap(ctx, parsed, "reply");   /* structure real; string/bool leaves concolic (fork + real example) */
}

/* PARKED reply consumers: r.json()/r.text() of a not-yet-fetched body hold their promise's resolve here and
   PARK — no opaque settle, no separate boot re-run. When qjs_provide caches the body, pendreply_resolve fires
   the resolve with the CONCOLIC reply value (structure real, leaves fork + carry the concrete example), so the
   continuation resumes with real data EXACTLY like a browser resolves fetch().then(r=>r.json()). A body that
   never arrives (fetch failed / not fetched) is resolved OPAQUE {reply} at finalize, preserving shape coverage. */
typedef struct { char *url; JSValue resolve; int is_json; } PendReply;
static PendReply *g_pendreply = NULL; static int g_pendreply_n = 0, g_pendreply_cap = 0;
static JSValue park_reply(JSContext *ctx, const char *url, int is_json) {
    JSValue rf[2]; JSValue promise = JS_NewPromiseCapability(ctx, rf);
    if (g_pendreply_n >= g_pendreply_cap) { int nc = g_pendreply_cap ? g_pendreply_cap * 2 : 8; PendReply *n = realloc(g_pendreply, (size_t)nc * sizeof(PendReply)); CHECK(n, "pendreply-oom: a dropped parked reply never resolves, losing the endpoint's real values"); g_pendreply = n; g_pendreply_cap = nc; }
    g_pendreply[g_pendreply_n].url = strdup(url); g_pendreply[g_pendreply_n].resolve = rf[0]; g_pendreply[g_pendreply_n].is_json = is_json; g_pendreply_n++;
    JS_FreeValue(ctx, rf[1]);   /* reject unused: a missing body resolves OPAQUE (shape), never rejects */
    return promise;
}
void pendreply_resolve(JSContext *ctx, const char *url, const char *body) {
    for (int i = 0; i < g_pendreply_n; ) {
        if (strcmp(g_pendreply[i].url, url) == 0) {
            JSValue bs = JS_NewString(ctx, body); JSValue v = reply_value(ctx, bs, g_pendreply[i].is_json); JS_FreeValue(ctx, bs);
            JSValue r = JS_Call(ctx, g_pendreply[i].resolve, JS_UNDEFINED, 1, (JSValueConst *)&v); JS_FreeValue(ctx, r); JS_FreeValue(ctx, v);
            JS_FreeValue(ctx, g_pendreply[i].resolve); free(g_pendreply[i].url);
            g_pendreply[i] = g_pendreply[--g_pendreply_n];
        } else i++;
    }
}
void pendreply_drain_opaque(JSContext *ctx) {   /* finalize: a body that never arrived resolves OPAQUE {reply} so its continuation still runs (shape) */
    for (int i = 0; i < g_pendreply_n; i++) {
        JSValue v = js_concolic(ctx, "{reply}", JS_UNDEFINED);
        JSValue r = JS_Call(ctx, g_pendreply[i].resolve, JS_UNDEFINED, 1, (JSValueConst *)&v); JS_FreeValue(ctx, r); JS_FreeValue(ctx, v);
        JS_FreeValue(ctx, g_pendreply[i].resolve); free(g_pendreply[i].url);
    }
    g_pendreply_n = 0;
}
/* r.json()/r.text(): a CACHED body resolves CONCOLIC (the boot re-run's path, make_response injected __body);
   else REGISTER the fetch (concrete url) and resolve OPAQUE in place. The reply's provision enqueues a forking
   boot re-run that re-runs boot with the reply now synchronously concolic — delivery is the re-run, never a
   parked promise (persistent async state outside the flow's COW delta, which is what leaked). */
static JSValue resp_consume(JSContext *ctx, JSValueConst this_val, int is_json) {
    JSValue body = resp_body_str(ctx, this_val);
    if (JS_IsString(body)) { JSValue v = reply_value(ctx, body, is_json); JS_FreeValue(ctx, body); return js_resolved(ctx, v); }
    JS_FreeValue(ctx, body);
    JSValue u = JS_GetPropertyStr(ctx, this_val, "url");
    const char *url = JS_IsString(u) ? JS_ToCString(ctx, u) : NULL;
    if (url && url[0] && !has_hole(url)) {
        reply_fetch_register(url, is_json);                 /* concrete url -> fetch it */
        JSValue p = park_reply(ctx, url, is_json);          /* PARK: the continuation resumes with the concrete concolic reply when qjs_provide delivers the body — browser-faithful fetch().then(r=>r.json()), no re-run, no opaque settle */
        JS_FreeCString(ctx, url); JS_FreeValue(ctx, u);
        return p;
    }
    if (url) JS_FreeCString(ctx, url);
    JS_FreeValue(ctx, u);
    return js_resolved(ctx, js_concolic(ctx, "{reply}", JS_UNDEFINED));   /* holey/unfetchable url: no concrete source, resolve opaque {reply} (shape) */
}
static JSValue js_resp_json(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) { return resp_consume(ctx, this_val, 1); }
static JSValue js_resp_text(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) { return resp_consume(ctx, this_val, 0); }
/* build a fetch Response whose identity is concrete (ok/status/url) but whose BODY is opaque. */
JSValue make_response(JSContext *ctx, const char *url) {
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
        JS_SetPropertyStr(ctx, h, "get", JS_NewCFunction(ctx, js_concolic_stub, "get", 1));
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
