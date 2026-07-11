/* The @H endpoint sink — the shared destination every request host-edge funnels into.
 *
 * record_endpoint appends one learned endpoint {method,url,params,headers,body} to g_endpoints (unless a
 * CANDIDATE flow is running — its request URLs are @S breakout artifacts, not real @H) and signals the emit to
 * the running flow (the WFQ progress value). fetch, XMLHttpRequest, and form/element submission all call it.
 * Decoupled from the scheduler: it signals via flow_emit_value() rather than touching the Flow record. */
#ifndef ENGINE_HOST_ENDPOINT_H
#define ENGINE_HOST_ENDPOINT_H

#include "quickjs.h"

/* The learned @H endpoints. Owned here; read by main.c's finalize (in-engine dedup + @RESULT assembly). */
extern JSValue g_endpoints;

void record_endpoint(JSContext *ctx, JSValue ep);   /* the shared @H sink (consumes `ep`) */
void capture_headers(JSContext *ctx, JSValueConst ep, JSValueConst hdrs);   /* build ep.headers from a Headers/plain object (concolic EXAMPLE), shared by fetch + XHR */
void endpoint_init(JSContext *ctx);                 /* fresh @H array (qjs_init) */
void endpoint_free(JSContext *ctx);                 /* teardown */

#endif
