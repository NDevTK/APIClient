/* The fetch() host edge — see fetch.h. */
#include <stdlib.h>
#include "core/loader/fetch.h"
#include "solver/reply.h"   /* make_response */
#include "platform/url.h"     /* build_query_params */
#include "solver/endpoint.h"   /* record_endpoint */

/* Borrowed from main.c (the @H recording side): the URL hole-solver, request-header capture, the shared
   endpoint sink, and the resolved-promise helper. */
extern void capture_headers(JSContext *ctx, JSValueConst ep, JSValueConst hdrs);
extern JSValue js_resolved(JSContext *ctx, JSValue val);

JSValue js_fetch(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    /* CONCOLIC: a URL built from a trusted-loaded reply is a symbol carrying the REAL computed URL as its
       example — use it, so a gated arm emits /api/billing/enterprise/acme-42 (the merge layer shapes it +
       records the value) instead of losing the gate-independent value to a {} shape. Pure-symbolic attacker
       input has no example -> the shape, as before. */
    const char *url = NULL;
    if (argc > 0) {
        JSValue exurl = JS_OpaqueExample(ctx, argv[0]);
        if (!JS_IsUndefined(exurl)) url = JS_ToCString(ctx, exurl);
        JS_FreeValue(ctx, exurl);
        if (!url) url = JS_ToCString(ctx, argv[0]);
    }
    /* HTTP method from the RequestInit (fetch(url,{method:'DELETE'})) — a big security signal (GET vs
       DELETE/POST). A concrete string only; opaque/missing options -> GET. */
    const char *method = NULL;
    if (argc > 1 && JS_IsObject(argv[1])) {
        JSValue m = JS_GetPropertyStr(ctx, argv[1], "method");
        if (JS_IsString(m)) method = JS_ToCString(ctx, m);   /* opaque options -> .method opaque (not a string) -> GET */
        JS_FreeValue(ctx, m);
    }
    if (!method && argc > 0 && JS_IsObject(argv[0]) && !JS_IsOpaque(argv[0])) {   /* fetch(new Request(url,{method})); an opaque URL is not a Request */
        JSValue m = JS_GetPropertyStr(ctx, argv[0], "method");
        if (JS_IsString(m)) method = JS_ToCString(ctx, m);
        JS_FreeValue(ctx, m);
    }
    char *usolved = url_solve_holes(ctx, url);   /* value-solving: {src} holes the flow fixed -> concrete key (path + query) */
    const char *eurl = usolved ? usolved : url;
    JSValue ep = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, ep, "method", JS_NewString(ctx, method ? method : "GET"));
    JS_SetPropertyStr(ctx, ep, "url", JS_NewString(ctx, eurl ? eurl : "?"));
    JS_SetPropertyStr(ctx, ep, "source", JS_NewString(ctx, "ast_analysis"));
    JSValue params = JS_NewArray(ctx);
    build_query_params(ctx, eurl, params);
    free(usolved);
    JS_SetPropertyStr(ctx, ep, "params", params);
    /* REQUIRED HEADERS + REQUEST BODY: part of the endpoint spec (replay). A plain-object `headers` ->
       ep.headers{name:value}; a POST/PATCH body (already stringified by the bundle; opaque fields -> {}
       shape) -> ep.body. Both control-flattened + capped so a value can't break the emitted line. */
    {
        /* init = the RequestInit (argv[1]) or a Request object (argv[0]). An OPAQUE argv[0] is the URL itself
           (opaque values ARE objects), NOT an init — reading its phantom .body/.headers invented a spurious
           body param on a plain GET(opaqueUrl). Exclude opaque here. */
        JSValueConst init = (argc > 1 && JS_IsObject(argv[1])) ? argv[1]
                          : ((argc > 0 && JS_IsObject(argv[0]) && !JS_IsOpaque(argv[0])) ? argv[0] : JS_UNDEFINED);
        if (JS_IsObject(init)) {
            JSValue hdrs = JS_GetPropertyStr(ctx, init, "headers");
            capture_headers(ctx, ep, hdrs);   /* shared with XHR: plain object / Headers __fields, concolic value */
            JS_FreeValue(ctx, hdrs);
            JSValue body = JS_GetPropertyStr(ctx, init, "body");
            if (!JS_IsUndefined(body) && !JS_IsNull(body)) {
                const char *bs = NULL;
                /* CONCOLIC body: a pre-stringified opaque (URLSearchParams(..).toString(), a concat) OR an
                   OBJECT body (FormData) whose toString() returns a concolic opaque -> invoke toString to get
                   the concolic serialization, then read its concrete EXAMPLE (the JS ToString coercion would
                   otherwise flatten the opaque to its shape before we see the example). */
                JSValue braw = JS_DupValue(ctx, body);
                if (JS_IsObject(braw) && !JS_IsOpaque(braw)) {
                    JSValue ts = JS_GetPropertyStr(ctx, braw, "toString");
                    if (JS_IsFunction(ctx, ts)) { JSValue r = JS_Call(ctx, ts, braw, 0, NULL); if (!JS_IsException(r)) { JS_FreeValue(ctx, braw); braw = r; } else { JSValue e = JS_GetException(ctx); JS_FreeValue(ctx, e); } }
                    JS_FreeValue(ctx, ts);
                }
                JSValue exbody = JS_OpaqueExample(ctx, braw);
                if (!JS_IsUndefined(exbody)) bs = JS_ToCString(ctx, exbody);
                JS_FreeValue(ctx, exbody);
                if (!bs) bs = JS_ToCString(ctx, braw);          /* else the shape (opaque) or a plain string */
                JS_FreeValue(ctx, braw);
                if (bs && bs[0]) {
                    char *bsolved = url_solve_holes(ctx, bs);   /* value-solve {src} body holes the flow fixed (JSON.stringify keeps the shape now) */
                    JS_SetPropertyStr(ctx, ep, "body", JS_NewString(ctx, bsolved ? bsolved : bs));
                    free(bsolved);
                }
                if (bs) JS_FreeCString(ctx, bs);
            }
            JS_FreeValue(ctx, body);
        }
    }
    record_endpoint(ctx, ep);   /* the shared @H sink (dedup at finalize) — fetch AND XMLHttpRequest use it */
    JSValue resp = make_response(ctx, url);
    if (url) JS_FreeCString(ctx, url);
    if (method) JS_FreeCString(ctx, method);
    return js_resolved(ctx, resp);
}
