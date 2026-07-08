/* XMLHttpRequest emulation — see xhr.h. */
#include <stdlib.h>
#include <string.h>
#include "xhr.h"
#include "opaque.h"   /* g_opaque, js_noop, js_opaque_stub */
#include "url.h"      /* build_query_params */
#include "endpoint.h"   /* record_endpoint */

/* Borrowed from main.c (the @H recording side, scheduler-coupled): the shared endpoint sink, the URL
   hole-solver, the request-header capture, and the event-listener registrar (onload -> driven). */
extern char *url_solve_holes(JSContext *ctx, const char *url);
extern void capture_headers(JSContext *ctx, JSValueConst ep, JSValueConst hdrs);
extern JSValue js_add_listener(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv);

static JSValue js_xhr_setheader(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (argc >= 2) {   /* store into __headers so js_xhr_send captures the auth/CSRF/custom headers */
        JSValue h = JS_GetPropertyStr(ctx, this_val, "__headers");
        if (!JS_IsObject(h)) { JS_FreeValue(ctx, h); h = JS_NewObject(ctx); JS_SetPropertyStr(ctx, this_val, "__headers", JS_DupValue(ctx, h)); }
        const char *k = JS_ToCString(ctx, argv[0]);
        if (k) { JS_SetPropertyStr(ctx, h, k, JS_DupValue(ctx, argv[1])); JS_FreeCString(ctx, k); }
        JS_FreeValue(ctx, h);
    }
    return JS_UNDEFINED;
}
static JSValue js_xhr_open(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (argc >= 1) JS_SetPropertyStr(ctx, this_val, "__method", JS_DupValue(ctx, argv[0]));
    if (argc >= 2) JS_SetPropertyStr(ctx, this_val, "__url", JS_DupValue(ctx, argv[1]));
    return JS_UNDEFINED;
}
static JSValue js_xhr_send(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    JSValue mv = JS_GetPropertyStr(ctx, this_val, "__method");
    JSValue uv = JS_GetPropertyStr(ctx, this_val, "__url");
    const char *method = JS_IsString(mv) ? JS_ToCString(ctx, mv) : NULL;
    char *url = NULL;
    { JSValue exurl = JS_OpaqueExample(ctx, uv); const char *u = NULL;   /* concolic URL -> real computed value */
      if (!JS_IsUndefined(exurl)) u = JS_ToCString(ctx, exurl);
      if (!u && (JS_IsString(uv) || JS_IsOpaque(uv))) u = JS_ToCString(ctx, uv);
      if (u) { url = strdup(u); JS_FreeCString(ctx, u); }
      JS_FreeValue(ctx, exurl); }
    if (url) {
        char *usolved = url_solve_holes(ctx, url); const char *eurl = usolved ? usolved : url;
        JSValue ep = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, ep, "method", JS_NewString(ctx, method ? method : "GET"));
        JS_SetPropertyStr(ctx, ep, "url", JS_NewString(ctx, eurl));
        JS_SetPropertyStr(ctx, ep, "source", JS_NewString(ctx, "ast_analysis"));
        JSValue params = JS_NewArray(ctx); build_query_params(ctx, eurl, params); JS_SetPropertyStr(ctx, ep, "params", params);
        if (argc >= 1 && !JS_IsUndefined(argv[0]) && !JS_IsNull(argv[0])) {
            const char *bs = JS_ToCString(ctx, argv[0]);
            if (bs && bs[0]) { char *bsolved = url_solve_holes(ctx, bs); JS_SetPropertyStr(ctx, ep, "body", JS_NewString(ctx, bsolved ? bsolved : bs)); free(bsolved); }
            if (bs) JS_FreeCString(ctx, bs);
        }
        { JSValue h = JS_GetPropertyStr(ctx, this_val, "__headers"); capture_headers(ctx, ep, h); JS_FreeValue(ctx, h); }
        record_endpoint(ctx, ep);
        free(usolved); free(url);
    }
    if (method) JS_FreeCString(ctx, method);
    JS_FreeValue(ctx, mv); JS_FreeValue(ctx, uv);
    return JS_UNDEFINED;
}
JSValue js_xhr_ctor(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv) {
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "open", JS_NewCFunction(ctx, js_xhr_open, "open", 2));
    JS_SetPropertyStr(ctx, o, "send", JS_NewCFunction(ctx, js_xhr_send, "send", 1));
    JS_SetPropertyStr(ctx, o, "setRequestHeader", JS_NewCFunction(ctx, js_xhr_setheader, "setRequestHeader", 2));
    JS_SetPropertyStr(ctx, o, "abort", JS_NewCFunction(ctx, js_noop, "abort", 0));
    JS_SetPropertyStr(ctx, o, "addEventListener", JS_NewCFunction(ctx, js_add_listener, "addEventListener", 2));   /* onload handler -> driven */
    JS_SetPropertyStr(ctx, o, "removeEventListener", JS_NewCFunction(ctx, js_noop, "removeEventListener", 2));
    JS_SetPropertyStr(ctx, o, "getResponseHeader", JS_NewCFunction(ctx, js_opaque_stub, "getResponseHeader", 1));
    JS_SetPropertyStr(ctx, o, "getAllResponseHeaders", JS_NewCFunction(ctx, js_opaque_stub, "getAllResponseHeaders", 0));
    JS_SetPropertyStr(ctx, o, "responseText", JS_DupValue(ctx, g_opaque));   /* response = external input */
    JS_SetPropertyStr(ctx, o, "response", JS_DupValue(ctx, g_opaque));
    JS_SetPropertyStr(ctx, o, "status", JS_NewInt32(ctx, 200));
    JS_SetPropertyStr(ctx, o, "readyState", JS_NewInt32(ctx, 4));
    return o;
}
