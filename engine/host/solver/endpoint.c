/* The @H endpoint sink — see endpoint.h. */
#include "solver/endpoint.h"

/* Borrowed from main.c (the scheduler side): the @S candidate flag (a candidate flow's requests are @S
   artifacts, not real @H), the emit counter, and the flow value-emit signal. */
extern char *g_candidate;
extern int g_emit_total;
extern void flow_emit_value(void);

JSValue g_endpoints = JS_UNDEFINED;   /* JS array of {method,url,params,headers,body} */

/* The shared @H sink: record one endpoint (consumes `ep`) + signal the emit. A CANDIDATE flow carries a
   concrete @S breakout PAYLOAD, so its request URLs are @S artifacts, NOT real @H — only OPAQUE flows emit.
   fetch AND XMLHttpRequest funnel through here so XHR-based apps are learned like fetch ones. */
void record_endpoint(JSContext *ctx, JSValue ep) {
    if (JS_IsArray(g_endpoints) && !g_candidate) {
        uint32_t n = 0; JSValue lv = JS_GetPropertyStr(ctx, g_endpoints, "length"); JS_ToUint32(ctx, &n, lv); JS_FreeValue(ctx, lv);
        JS_SetPropertyUint32(ctx, g_endpoints, n, ep);   /* consumes ep */
        g_emit_total++;
    } else {
        JS_FreeValue(ctx, ep);
    }
    flow_emit_value();   /* raise the running flow's value (the WFQ progress signal) — no Flow struct needed here */
}

void endpoint_init(JSContext *ctx) { g_endpoints = JS_NewArray(ctx); }
void endpoint_free(JSContext *ctx) { JS_FreeValue(ctx, g_endpoints); g_endpoints = JS_UNDEFINED; }

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
