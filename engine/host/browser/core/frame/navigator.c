/* Network-initiating navigator methods — see navigator.h. Extracted from main.c. sendBeacon(url,data) is a
 * real POST (analytics/telemetry endpoint) emitted like fetch; serviceWorker.register(url) fetches + analyzes
 * the SW script as a chunk. A state-mutating POST is never fired to learn — the beacon body's example comes
 * ONLY from this forced-exec serialize (url_solve_holes fills == gate-pinned holes). */
#include <stdlib.h>
#include "core/frame/navigator.h"
#include "platform/url.h"        /* url_from_arg, url_solve_holes, has_hole, build_query_params */
#include "endpoint.h"   /* record_endpoint — the shared @H sink */
#include "opaque.h"     /* g_opaque */

extern void chunk_pending_add(const char *url);              /* scheduler-side (main.c): queue a script chunk for host fetch + analyze */
extern JSValue js_resolved(JSContext *ctx, JSValue val);     /* scheduler-side: wrap a value in a resolved promise */
extern JSValue js_add_listener(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv);   /* serviceWorker.onmessage -> driven flow */

JSValue js_sw_register(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (argc >= 1) {
        char *url = url_from_arg(ctx, argv[0]);
        if (url) { if (!has_hole(url)) chunk_pending_add(url); free(url); }   /* -> host fetch + engine analyze */
    }
    return js_resolved(ctx, js_concolic(ctx, "{swRegistration}", JS_UNDEFINED));   /* Promise<ServiceWorkerRegistration> */
}

/* window.navigator — the standard properties as CONCOLIC values (a real desktop Chrome as the example), the
   network-initiating methods (sendBeacon/serviceWorker), and the opaque sentinel as prototype so a genuinely
   device-dependent member (clipboard/geolocation/getBattery) still falls through to opaque. */
JSValue js_navigator_make(JSContext *ctx) {
    JSValue nav = JS_NewObject(ctx);
    const char *UA = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36";
    JS_SetPropertyStr(ctx, nav, "userAgent", js_concolic(ctx, "{ua}", JS_NewString(ctx, UA)));
    JS_SetPropertyStr(ctx, nav, "appVersion", js_concolic(ctx, "{ua}", JS_NewString(ctx, UA + 8)));   /* appVersion = UA minus "Mozilla/" */
    JS_SetPropertyStr(ctx, nav, "appName", JS_NewString(ctx, "Netscape"));
    JS_SetPropertyStr(ctx, nav, "appCodeName", JS_NewString(ctx, "Mozilla"));
    JS_SetPropertyStr(ctx, nav, "product", JS_NewString(ctx, "Gecko"));
    JS_SetPropertyStr(ctx, nav, "productSub", JS_NewString(ctx, "20030107"));
    JS_SetPropertyStr(ctx, nav, "vendor", js_concolic(ctx, "{vendor}", JS_NewString(ctx, "Google Inc.")));
    JS_SetPropertyStr(ctx, nav, "vendorSub", JS_NewString(ctx, ""));
    JS_SetPropertyStr(ctx, nav, "platform", js_concolic(ctx, "{platform}", JS_NewString(ctx, "Win32")));
    JS_SetPropertyStr(ctx, nav, "language", js_concolic(ctx, "{lang}", JS_NewString(ctx, "en-US")));
    { JSValue langs = JS_NewArray(ctx); JS_SetPropertyUint32(ctx, langs, 0, JS_NewString(ctx, "en-US")); JS_SetPropertyUint32(ctx, langs, 1, JS_NewString(ctx, "en")); JS_SetPropertyStr(ctx, nav, "languages", langs); }
    JS_SetPropertyStr(ctx, nav, "onLine", js_concolic(ctx, "{online}", JS_TRUE));                     /* forks online/offline; example true */
    JS_SetPropertyStr(ctx, nav, "cookieEnabled", js_concolic(ctx, "{cookieEnabled}", JS_TRUE));
    JS_SetPropertyStr(ctx, nav, "webdriver", js_concolic(ctx, "{webdriver}", JS_FALSE));              /* a real user browser -> false; the bot-gate arm forks too */
    JS_SetPropertyStr(ctx, nav, "doNotTrack", JS_NULL);
    JS_SetPropertyStr(ctx, nav, "hardwareConcurrency", js_concolic(ctx, "{cores}", JS_NewInt32(ctx, 8)));
    JS_SetPropertyStr(ctx, nav, "deviceMemory", js_concolic(ctx, "{mem}", JS_NewInt32(ctx, 8)));
    JS_SetPropertyStr(ctx, nav, "maxTouchPoints", js_concolic(ctx, "{touch}", JS_NewInt32(ctx, 0)));
    JS_SetPropertyStr(ctx, nav, "pdfViewerEnabled", JS_TRUE);
    JS_SetPropertyStr(ctx, nav, "sendBeacon", JS_NewCFunction(ctx, js_send_beacon, "sendBeacon", 2));
    {   /* serviceWorker.register(url) -> analyze the SW script (its endpoints); other members -> opaque */
        JSValue sw = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, sw, "register", JS_NewCFunction(ctx, js_sw_register, "register", 1));
        JS_SetPropertyStr(ctx, sw, "addEventListener", JS_NewCFunction(ctx, js_add_listener, "addEventListener", 2));
        JS_SetPropertyStr(ctx, sw, "ready", js_resolved(ctx, js_concolic(ctx, "{swRegistration}", JS_UNDEFINED)));
        JS_SetPrototype(ctx, sw, g_opaque);
        JS_SetPropertyStr(ctx, nav, "serviceWorker", sw);
    }
    JS_SetPrototype(ctx, nav, g_opaque);   /* genuinely device-dependent members (clipboard/geolocation/getBattery) -> opaque */
    return nav;
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
