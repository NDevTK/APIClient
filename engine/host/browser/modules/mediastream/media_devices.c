/* MediaDevices module — see media_devices.h. */
#include "modules/mediastream/media_devices.h"
#include "bindings/idl.h"          /* idl_dfail_wrap — unbuilt MediaDevices members DFAIL */
#include "solver/concolic.h"       /* js_concolic, js_noop */

#include "platform/promise.h"
extern JSValue js_add_listener(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv);       /* 'devicechange' -> driven flow */

/* getUserMedia/getDisplayMedia: Promise<MediaStream> — the stream is a concolic (its track labels/settings are
   permission-gated, unknowable), so `s.getTracks()[0].label` flows concolic to a sink/endpoint. */
static JSValue md_get_stream(JSContext *ctx, JSValueConst t, int c, JSValueConst *v) {
    (void)t; (void)c; (void)v;
    return js_resolved(ctx, js_concolic(ctx, "{mediaStream}", JS_UNDEFINED));
}
/* enumerateDevices(): Promise<sequence<MediaDeviceInfo>> — a concolic device collection. `.forEach`/`for-of` over
   it drives the body with a concolic device whose deviceId/label/groupId are permission-gated sources. */
static JSValue md_enumerate(JSContext *ctx, JSValueConst t, int c, JSValueConst *v) {
    (void)t; (void)c; (void)v;
    return js_resolved(ctx, js_concolic(ctx, "{mediaDevices}", JS_UNDEFINED));
}
/* getSupportedConstraints(): MediaTrackSupportedConstraints — concolic so `if (caps.facingMode)` forks the
   feature-detection branch (a mobile/desktop-style capability split). */
static JSValue md_supported(JSContext *ctx, JSValueConst t, int c, JSValueConst *v) {
    (void)t; (void)c; (void)v;
    return js_concolic(ctx, "{supportedConstraints}", JS_UNDEFINED);
}

JSValue media_devices_make(JSContext *ctx) {
    JSValue m = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, m, "getUserMedia", JS_NewCFunction(ctx, md_get_stream, "getUserMedia", 1));
    JS_SetPropertyStr(ctx, m, "getDisplayMedia", JS_NewCFunction(ctx, md_get_stream, "getDisplayMedia", 1));
    JS_SetPropertyStr(ctx, m, "enumerateDevices", JS_NewCFunction(ctx, md_enumerate, "enumerateDevices", 0));
    JS_SetPropertyStr(ctx, m, "getSupportedConstraints", JS_NewCFunction(ctx, md_supported, "getSupportedConstraints", 0));
    JS_SetPropertyStr(ctx, m, "addEventListener", JS_NewCFunction(ctx, js_add_listener, "addEventListener", 2));   /* 'devicechange' */
    JS_SetPropertyStr(ctx, m, "removeEventListener", JS_NewCFunction(ctx, js_noop, "removeEventListener", 2));
    return idl_dfail_wrap(ctx, m, "MediaDevices");
}
