/* Fetch Response + reply-body learning — see reply.h. */
#include "solver/reply.h"
#include "solver/concolic.h"   /* g_concolic, js_concolic_stub */
#include "solver/parked.h"     /* the ONE async delivery-park mechanism, shared with dynamic import */
#include "platform/url.h"      /* has_hole */
#include "check.h"             /* CHECK — a dropped parked reply never resolves, losing the endpoint */
#include <string.h>
#include <stdlib.h>

/* The reply-body CACHE: { url -> concrete reply body text }. Host-seeded at init (the fromReply table a real
   GET already fetched) and extended by qjs_provide as bodies arrive; make_response reads it so a re-entrant /
   cached r.json()/r.text() returns the CONCRETE reply synchronously (not opaque). Owned here — the reply
   component owns the whole reply concern (cache + park-resume delivery + fetch registry edge). */
JSValue g_reply_table = JS_UNDEFINED;
extern JSValue js_resolved(JSContext *ctx, JSValue val);
extern void reply_fetch_register(const char *url, int is_json);

/* Seed the cache from the host's fromReply JSON (a real GET already fetched these bodies). */
void reply_cache_seed(JSContext *ctx, const char *replies_json) {
    if (!replies_json || !replies_json[0]) return;
    JSValue t = JS_ParseJSON(ctx, replies_json, strlen(replies_json), "<replies>");
    if (JS_IsException(t)) { JSValue e = JS_GetException(ctx); JS_FreeValue(ctx, e); }
    else { JS_FreeValue(ctx, g_reply_table); g_reply_table = t; }
}
/* Cache a fetched body under its url (creating the table if the host seeded none) — a re-run's make_response
   then injects __body so r.json()/r.text() are CONCOLIC synchronously. */
void reply_cache_put(JSContext *ctx, const char *url, const char *body) {
    if (!url || !body) return;
    if (!JS_IsObject(g_reply_table)) { JS_FreeValue(ctx, g_reply_table); g_reply_table = JS_NewObject(ctx); }
    JS_SetPropertyStr(ctx, g_reply_table, url, JS_NewString(ctx, body));
}
void reply_cache_free(JSContext *ctx) { JS_FreeValue(ctx, g_reply_table); g_reply_table = JS_UNDEFINED; }

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

/* PARKED reply consumers: r.json()/r.text() of a not-yet-fetched body PARKS its promise's resolve in the shared
   delivery-park (solver/parked, ONE mechanism shared with dynamic import). When qjs_provide caches the body,
   pendreply_resolve fires each parked reply with the CONCOLIC reply value (structure real, leaves fork + carry
   the concrete example), so the continuation resumes with real data EXACTLY like a browser resolves
   fetch().then(r=>r.json()). A body that never arrives resolves OPAQUE {reply} at finalize (shape coverage). The
   per-entry `tag` carries is_json. */
static ParkTable *g_reply_park = NULL;
static JSValue park_reply(JSContext *ctx, const char *url, int is_json) {
    JSValue rf[2]; JSValue promise = JS_NewPromiseCapability(ctx, rf);
    if (!g_reply_park) g_reply_park = park_new();
    park_add(ctx, g_reply_park, url, rf[0], is_json);
    JS_FreeValue(ctx, rf[0]); JS_FreeValue(ctx, rf[1]);   /* park dups resolve; reject unused (a missing body resolves OPAQUE, never rejects) */
    return promise;
}
static JSValue reply_val_compute(JSContext *ctx, const char *url, int tag, void *ud) {   /* value = the concolic reply parsed per is_json (tag) */
    (void)url; const char *body = (const char *)ud;
    JSValue bs = JS_NewString(ctx, body); JSValue v = reply_value(ctx, bs, tag); JS_FreeValue(ctx, bs); return v;
}
static JSValue reply_opaque_compute(JSContext *ctx, const char *url, int tag, void *ud) {   /* never-delivered: opaque {reply} */
    (void)url; (void)tag; (void)ud; return js_concolic(ctx, "{reply}", JS_UNDEFINED);
}
void pendreply_resolve(JSContext *ctx, const char *url, const char *body) { park_resolve_url(ctx, g_reply_park, url, reply_val_compute, (void *)body); }
void pendreply_drain_opaque(JSContext *ctx) { park_drain(ctx, g_reply_park, reply_opaque_compute, NULL); }
void pendreply_free(JSContext *ctx) { park_free(ctx, g_reply_park); g_reply_park = NULL; }
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
