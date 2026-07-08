/* The @H endpoint sink — see endpoint.h. */
#include "endpoint.h"

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
