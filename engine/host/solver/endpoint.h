/* The @H endpoint sink — the shared destination every request host-edge funnels into.
 *
 * record_endpoint EXTRACTS one learned endpoint {method,url,params,headers,body} into a typed C struct (unless
 * a CANDIDATE flow is running — its request URLs are @S breakout artifacts, not real @H) and signals the emit
 * to the running flow (the WFQ progress value). fetch, XMLHttpRequest, and form/element submission all call it.
 * Decoupled from the scheduler: it signals via flow_emit_value() rather than touching the Flow record. The
 * findings are C DATA (COW-invisible by construction), not JS-heap objects — endpoint_snapshot rebuilds a
 * transient JS array for the in-engine dedup at emit. */
#ifndef ENGINE_HOST_ENDPOINT_H
#define ENGINE_HOST_ENDPOINT_H

#include "quickjs.h"

void record_endpoint(JSContext *ctx, JSValue ep);   /* the shared @H sink (consumes `ep`) */
JSValue endpoint_snapshot(JSContext *ctx);          /* rebuild a JS array of the C findings for dedup/emit (caller frees) */
void capture_headers(JSContext *ctx, JSValueConst ep, JSValueConst hdrs);   /* build ep.headers from a Headers/plain object (concolic EXAMPLE), shared by fetch + XHR */
void endpoint_init(JSContext *ctx);                 /* fresh @H array (qjs_init) */
void endpoint_free(JSContext *ctx);                 /* teardown */

#endif
