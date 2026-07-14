/* The @H endpoint sink — see endpoint.h.
 *
 * HOST FINDINGS ARE C DATA — NOT JS-HEAP OBJECTS. A learned endpoint is stored in a typed C struct
 * (Endpoint), fully OUTSIDE the JS heap, so it is COW-invisible BY CONSTRUCTION: the per-flow COW delta
 * captures baseline page-heap objects, and a C struct is not one, so it can never be captured/reverted when
 * the recording flow parks (an awaited fetch — heavy_async) or hands off. This is the principled replacement
 * for the deleted per-object `cow_exempt` flag (a manual opt-out easy to forget): host analysis state simply
 * does not live in the captured heap. `record_endpoint` EXTRACTS the transient JS `ep` into the C struct at
 * record time (when its values are live, before any revert); `endpoint_snapshot` rebuilds a JS array for the
 * in-engine dedup at emit (transient, flow_local, never captured). */
#include "solver/endpoint.h"
#include <stdlib.h>
#include <string.h>

/* Borrowed from main.c (the scheduler side): the @S candidate flag (a candidate flow's requests are @S
   artifacts, not real @H), the emit counter, and the flow value-emit signal. */
extern char *g_candidate;
extern int g_emit_total;
extern void flow_emit_value(void);

typedef struct { char *name; char *location; char **vals; int vals_n; } EpParam;
typedef struct { char *name; char *value; } EpHeader;
typedef struct {
    char *method, *url, *source, *body;
    EpParam *params; int params_n;
    EpHeader *headers; int headers_n;
} Endpoint;

static Endpoint *g_eps = NULL;      /* the learned @H endpoints — C data, COW-invisible by construction */
static int g_eps_n = 0, g_eps_cap = 0;

/* dup a string-valued own property to a heap C string (NULL if absent/non-string). */
static char *ep_str_prop(JSContext *ctx, JSValueConst o, const char *k) {
    JSValue v = JS_GetPropertyStr(ctx, o, k);
    char *r = NULL;
    if (JS_IsString(v)) { const char *s = JS_ToCString(ctx, v); if (s) { r = strdup(s); JS_FreeCString(ctx, s); } }
    JS_FreeValue(ctx, v);
    return r;
}

/* The shared @H sink: record one endpoint (consumes `ep`) + signal the emit. A CANDIDATE flow carries a
   concrete @S breakout PAYLOAD, so its request URLs are @S artifacts, NOT real @H — only OPAQUE flows emit.
   fetch AND XMLHttpRequest funnel through here so XHR-based apps are learned like fetch ones. The JS `ep` is
   EXTRACTED into a C Endpoint here and then freed — nothing of the finding survives in the JS heap. */
void record_endpoint(JSContext *ctx, JSValue ep) {
    if (!g_candidate && JS_IsObject(ep)) {
        if (g_eps_n >= g_eps_cap) { g_eps_cap = g_eps_cap ? g_eps_cap * 2 : 16; g_eps = realloc(g_eps, (size_t)g_eps_cap * sizeof(Endpoint)); }
        Endpoint *e = &g_eps[g_eps_n];
        memset(e, 0, sizeof *e);
        e->method = ep_str_prop(ctx, ep, "method");
        e->url    = ep_str_prop(ctx, ep, "url");
        e->source = ep_str_prop(ctx, ep, "source");
        e->body   = ep_str_prop(ctx, ep, "body");
        /* params: [{name, location, validValues:[...]}] */
        JSValue params = JS_GetPropertyStr(ctx, ep, "params");
        if (JS_IsArray(params)) {
            uint32_t pn = 0; JSValue plv = JS_GetPropertyStr(ctx, params, "length"); JS_ToUint32(ctx, &pn, plv); JS_FreeValue(ctx, plv);
            if (pn) e->params = calloc(pn, sizeof(EpParam));
            for (uint32_t i = 0; i < pn; i++) {
                JSValue po = JS_GetPropertyUint32(ctx, params, i);
                EpParam *p = &e->params[e->params_n];
                p->name = ep_str_prop(ctx, po, "name");
                p->location = ep_str_prop(ctx, po, "location");
                JSValue vv = JS_GetPropertyStr(ctx, po, "validValues");
                if (JS_IsArray(vv)) {
                    uint32_t vn = 0; JSValue vlv = JS_GetPropertyStr(ctx, vv, "length"); JS_ToUint32(ctx, &vn, vlv); JS_FreeValue(ctx, vlv);
                    if (vn) p->vals = calloc(vn, sizeof(char *));
                    for (uint32_t j = 0; j < vn; j++) {
                        JSValue el = JS_GetPropertyUint32(ctx, vv, j);
                        if (JS_IsString(el)) { const char *s = JS_ToCString(ctx, el); if (s) { p->vals[p->vals_n++] = strdup(s); JS_FreeCString(ctx, s); } }
                        JS_FreeValue(ctx, el);
                    }
                }
                JS_FreeValue(ctx, vv);
                e->params_n++;
                JS_FreeValue(ctx, po);
            }
        }
        JS_FreeValue(ctx, params);
        /* headers: {name: value} */
        JSValue hdrs = JS_GetPropertyStr(ctx, ep, "headers");
        if (JS_IsObject(hdrs)) {
            JSPropertyEnum *tab = NULL; uint32_t hn = 0;
            if (JS_GetOwnPropertyNames(ctx, &tab, &hn, hdrs, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) == 0) {
                if (hn) e->headers = calloc(hn, sizeof(EpHeader));
                for (uint32_t i = 0; i < hn; i++) {
                    const char *hk = JS_AtomToCString(ctx, tab[i].atom);
                    JSValue hv = JS_GetProperty(ctx, hdrs, tab[i].atom);
                    const char *hvs = JS_IsString(hv) ? JS_ToCString(ctx, hv) : NULL;
                    if (hk && hvs) { EpHeader *h = &e->headers[e->headers_n++]; h->name = strdup(hk); h->value = strdup(hvs); }
                    if (hk) JS_FreeCString(ctx, hk);
                    if (hvs) JS_FreeCString(ctx, hvs);
                    JS_FreeValue(ctx, hv);
                }
                JS_FreePropertyEnum(ctx, tab, hn);
            }
        }
        JS_FreeValue(ctx, hdrs);
        g_eps_n++;
        g_emit_total++;
    }
    JS_FreeValue(ctx, ep);
    flow_emit_value();   /* raise the running flow's value (the WFQ progress signal) */
}

/* Rebuild a JS array of {method,url,source,params,headers,body} from the C findings for the in-engine dedup
   + @RESULT assembly at emit. Transient (created outside any capturing context, freed by the caller). */
JSValue endpoint_snapshot(JSContext *ctx) {
    JSValue arr = JS_NewArray(ctx);
    for (int i = 0; i < g_eps_n; i++) {
        Endpoint *e = &g_eps[i];
        JSValue ep = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, ep, "method", JS_NewString(ctx, e->method ? e->method : "GET"));
        JS_SetPropertyStr(ctx, ep, "url", JS_NewString(ctx, e->url ? e->url : "?"));
        JS_SetPropertyStr(ctx, ep, "source", JS_NewString(ctx, e->source ? e->source : "ast_analysis"));
        if (e->body) JS_SetPropertyStr(ctx, ep, "body", JS_NewString(ctx, e->body));
        JSValue params = JS_NewArray(ctx);
        for (int j = 0; j < e->params_n; j++) {
            EpParam *p = &e->params[j];
            JSValue po = JS_NewObject(ctx);
            JS_SetPropertyStr(ctx, po, "name", JS_NewString(ctx, p->name ? p->name : ""));
            JS_SetPropertyStr(ctx, po, "location", JS_NewString(ctx, p->location ? p->location : "query"));
            JSValue vv = JS_NewArray(ctx);
            for (int k = 0; k < p->vals_n; k++) JS_SetPropertyUint32(ctx, vv, (uint32_t)k, JS_NewString(ctx, p->vals[k]));
            JS_SetPropertyStr(ctx, po, "validValues", vv);
            JS_SetPropertyUint32(ctx, params, (uint32_t)j, po);
        }
        JS_SetPropertyStr(ctx, ep, "params", params);
        if (e->headers_n) {
            JSValue hobj = JS_NewObject(ctx);
            for (int j = 0; j < e->headers_n; j++) JS_SetPropertyStr(ctx, hobj, e->headers[j].name, JS_NewString(ctx, e->headers[j].value));
            JS_SetPropertyStr(ctx, ep, "headers", hobj);
        }
        JS_SetPropertyUint32(ctx, arr, (uint32_t)i, ep);
    }
    return arr;
}

static void endpoint_reset(void) {
    for (int i = 0; i < g_eps_n; i++) {
        Endpoint *e = &g_eps[i];
        free(e->method); free(e->url); free(e->source); free(e->body);
        for (int j = 0; j < e->params_n; j++) { free(e->params[j].name); free(e->params[j].location); for (int k = 0; k < e->params[j].vals_n; k++) free(e->params[j].vals[k]); free(e->params[j].vals); }
        free(e->params);
        for (int j = 0; j < e->headers_n; j++) { free(e->headers[j].name); free(e->headers[j].value); }
        free(e->headers);
    }
    free(g_eps); g_eps = NULL; g_eps_n = 0; g_eps_cap = 0;
}

void endpoint_init(JSContext *ctx) { (void)ctx; endpoint_reset(); }
void endpoint_free(JSContext *ctx) { (void)ctx; endpoint_reset(); }

/* Capture request header name:value pairs into ep.headers (required-headers replay spec). Reads a plain object
   OR a `new Headers()`'s __fields, and a concolic value's EXAMPLE (`'Bearer '+token` -> the real token). ONE
   home for fetch + XHR (no duplication) — the header side of building an @H endpoint record, so it lives with
   the endpoint sink, not the engine entry. */
void capture_headers(JSContext *ctx, JSValueConst ep, JSValueConst hdrs) {
    if (!JS_IsObject(hdrs) || JS_IsConcolic(hdrs)) return;
    JSValue hf = JS_GetPropertyStr(ctx, hdrs, "__fields");
    JSValueConst hsrc = JS_IsObject(hf) ? (JSValueConst)hf : hdrs;
    JSValue hobj = JS_NewObject(ctx); int any = 0;
    JSPropertyEnum *tab = NULL; uint32_t hn = 0;
    if (JS_GetOwnPropertyNames(ctx, &tab, &hn, hsrc, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) == 0) {
        for (uint32_t hi = 0; hi < hn; hi++) {
            const char *hk = JS_AtomToCString(ctx, tab[hi].atom);
            JSValue hv = JS_GetProperty(ctx, hsrc, tab[hi].atom);
            JSValue hex = JS_IsConcolic(hv) ? JS_ConcolicExample(ctx, hv) : JS_UNDEFINED;
            const char *hvs = !JS_IsUndefined(hex) ? JS_ToCString(ctx, hex) : JS_ToCString(ctx, hv);
            JS_FreeValue(ctx, hex);
            if (hk && hvs) { JS_SetPropertyStr(ctx, hobj, hk, JS_NewString(ctx, hvs)); any = 1; }
            if (hk) JS_FreeCString(ctx, hk);
            if (hvs) JS_FreeCString(ctx, hvs);
            JS_FreeValue(ctx, hv);
        }
        JS_FreePropertyEnum(ctx, tab, hn);
    }
    JS_FreeValue(ctx, hf);
    if (any) JS_SetPropertyStr(ctx, ep, "headers", hobj); else JS_FreeValue(ctx, hobj);
}
