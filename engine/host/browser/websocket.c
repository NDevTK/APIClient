/* WebSocket + EventSource — see websocket.h. Extracted from main.c. The handshake is a GET whose URL is an
 * ENDPOINT (emit it); the object exposes send/close/addEventListener, and the onmessage handler is registered
 * as a scheduler FLOW (js_add_listener) so a never-fired handler is still orphan-driven. */
#include <stdlib.h>
#include "websocket.h"
#include "url.h"        /* url_from_arg, url_solve_holes, build_query_params */
#include "endpoint.h"   /* record_endpoint — the shared @H sink */
#include "opaque.h"     /* js_noop */

extern JSValue js_add_listener(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv);   /* scheduler-side (main.c): onmessage -> driven */

JSValue js_ws_ctor(JSContext *ctx, JSValueConst nt, int argc, JSValueConst *argv) {
    if (argc >= 1) {
        char *url = url_from_arg(ctx, argv[0]);
        if (url) {
            char *usolved = url_solve_holes(ctx, url); const char *eurl = usolved ? usolved : url;
            JSValue ep = JS_NewObject(ctx);
            JS_SetPropertyStr(ctx, ep, "method", JS_NewString(ctx, "GET"));
            JS_SetPropertyStr(ctx, ep, "url", JS_NewString(ctx, eurl));
            JS_SetPropertyStr(ctx, ep, "source", JS_NewString(ctx, "ast_analysis"));
            JSValue params = JS_NewArray(ctx); build_query_params(ctx, eurl, params); JS_SetPropertyStr(ctx, ep, "params", params);
            record_endpoint(ctx, ep); free(usolved); free(url);
        }
    }
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "send", JS_NewCFunction(ctx, js_noop, "send", 1));
    JS_SetPropertyStr(ctx, o, "close", JS_NewCFunction(ctx, js_noop, "close", 0));
    JS_SetPropertyStr(ctx, o, "addEventListener", JS_NewCFunction(ctx, js_add_listener, "addEventListener", 2));
    JS_SetPropertyStr(ctx, o, "removeEventListener", JS_NewCFunction(ctx, js_noop, "removeEventListener", 2));
    JS_SetPropertyStr(ctx, o, "readyState", JS_NewInt32(ctx, 1));
    return o;
}
