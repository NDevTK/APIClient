/* NavigatorUAData (UA Client Hints) — see navigator_ua_data.h. */
#include "core/frame/navigator_ua_data.h"
#include "bindings/idl.h"        /* idl_dfail_wrap — unbuilt NavigatorUAData members DFAIL */
#include "solver/concolic.h"     /* js_concolic */

extern JSValue js_resolved(JSContext *ctx, JSValue val);   /* scheduler-side */

/* getHighEntropyValues(hints): resolves to a concolic detail object; each field forks its feature-detection. */
static JSValue ua_high_entropy(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "platform", js_concolic(ctx, "{uaPlatform}", JS_NewString(ctx, "Windows")));
    JS_SetPropertyStr(ctx, o, "platformVersion", js_concolic(ctx, "{uaPlatformVersion}", JS_NewString(ctx, "15.0.0")));
    JS_SetPropertyStr(ctx, o, "architecture", js_concolic(ctx, "{uaArch}", JS_NewString(ctx, "x86")));
    JS_SetPropertyStr(ctx, o, "bitness", js_concolic(ctx, "{uaBitness}", JS_NewString(ctx, "64")));
    JS_SetPropertyStr(ctx, o, "model", js_concolic(ctx, "{uaModel}", JS_NewString(ctx, "")));
    JS_SetPropertyStr(ctx, o, "uaFullVersion", js_concolic(ctx, "{uaFullVersion}", JS_NewString(ctx, "120.0.0.0")));
    JS_SetPropertyStr(ctx, o, "mobile", js_concolic(ctx, "{uaMobile}", JS_FALSE));
    return js_resolved(ctx, o);
}

JSValue navigator_ua_data_make(JSContext *ctx) {
    JSValue u = JS_NewObject(ctx);
    { JSValue brands = JS_NewArray(ctx), b0 = JS_NewObject(ctx);
      JS_SetPropertyStr(ctx, b0, "brand", JS_NewString(ctx, "Chromium")); JS_SetPropertyStr(ctx, b0, "version", JS_NewString(ctx, "120"));
      JS_SetPropertyUint32(ctx, brands, 0, b0); JS_SetPropertyStr(ctx, u, "brands", brands); }
    JS_SetPropertyStr(ctx, u, "mobile", js_concolic(ctx, "{uaMobile}", JS_FALSE));            /* forks mobile/desktop code paths */
    JS_SetPropertyStr(ctx, u, "platform", js_concolic(ctx, "{uaPlatform}", JS_NewString(ctx, "Windows")));
    JS_SetPropertyStr(ctx, u, "getHighEntropyValues", JS_NewCFunction(ctx, ua_high_entropy, "getHighEntropyValues", 1));
    return idl_dfail_wrap(ctx, u, "NavigatorUAData");   /* unbuilt members (toJSON, ...) DFAIL */
}
