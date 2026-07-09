/* Network-initiating navigator methods — see navigator.h. Extracted from main.c. sendBeacon(url,data) is a
 * real POST (analytics/telemetry endpoint) emitted like fetch; serviceWorker.register(url) fetches + analyzes
 * the SW script as a chunk. A state-mutating POST is never fired to learn — the beacon body's example comes
 * ONLY from this forced-exec serialize (url_solve_holes fills == gate-pinned holes). */
#include <stdlib.h>
#include "navigator.h"
#include "url.h"        /* url_from_arg, url_solve_holes, has_hole, build_query_params */
#include "endpoint.h"   /* record_endpoint — the shared @H sink */
#include "opaque.h"     /* g_opaque */

extern void chunk_pending_add(const char *url);              /* scheduler-side (main.c): queue a script chunk for host fetch + analyze */
extern JSValue js_resolved(JSContext *ctx, JSValue val);     /* scheduler-side: wrap a value in a resolved promise */

JSValue js_sw_register(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (argc >= 1) {
        char *url = url_from_arg(ctx, argv[0]);
        if (url) { if (!has_hole(url)) chunk_pending_add(url); free(url); }   /* -> host fetch + engine analyze */
    }
    return js_resolved(ctx, JS_DupValue(ctx, g_opaque));   /* Promise<ServiceWorkerRegistration> */
}

JSValue js_send_beacon(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (argc >= 1) {
        char *url = url_from_arg(ctx, argv[0]);
        if (url) {
            char *usolved = url_solve_holes(ctx, url); const char *eurl = usolved ? usolved : url;
            JSValue ep = JS_NewObject(ctx);
            JS_SetPropertyStr(ctx, ep, "method", JS_NewString(ctx, "POST"));
            JS_SetPropertyStr(ctx, ep, "url", JS_NewString(ctx, eurl));
            JS_SetPropertyStr(ctx, ep, "source", JS_NewString(ctx, "ast_analysis"));
            JSValue params = JS_NewArray(ctx); build_query_params(ctx, eurl, params); JS_SetPropertyStr(ctx, ep, "params", params);
            if (argc >= 2 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1])) {
                const char *bs = JS_ToCString(ctx, argv[1]);
                if (bs && bs[0]) { char *bsolved = url_solve_holes(ctx, bs); JS_SetPropertyStr(ctx, ep, "body", JS_NewString(ctx, bsolved ? bsolved : bs)); free(bsolved); }
                if (bs) JS_FreeCString(ctx, bs);
            }
            record_endpoint(ctx, ep); free(usolved); free(url);
        }
    }
    return JS_TRUE;
}
