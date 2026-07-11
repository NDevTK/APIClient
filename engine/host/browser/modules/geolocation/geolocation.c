/* Geolocation module — see geolocation.h. */
#include "modules/geolocation/geolocation.h"
#include "bindings/idl.h"          /* idl_dfail_wrap — unbuilt Geolocation members DFAIL */
#include "solver/concolic.h"       /* js_concolic, js_noop */
#include "solver/scheduler.h"      /* drive_opaque_cb — success(position) becomes a driven flow */

/* getCurrentPosition(success, error?, options?) / watchPosition(...): drive the success callback as a BFS flow
   with a concolic GeolocationPosition. position.coords.* is concolic (permission-gated, unknowable) so a branch
   on it forks and its taint reaches an endpoint. The position is sourced {geolocation}; property reads
   (.coords.latitude) synthesize concolic children, exactly as for any opaque object. */
static void drive_success(JSContext *ctx, int argc, JSValueConst *argv) {
    if (argc >= 1 && JS_IsFunction(ctx, argv[0])) {
        JSValue pos = js_concolic(ctx, "{geolocation}", JS_UNDEFINED);   /* GeolocationPosition — coords unknowable */
        drive_opaque_cb(ctx, argv[0], pos);
        JS_FreeValue(ctx, pos);
    }
}
static JSValue geo_get_current(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; drive_success(ctx, argc, argv);
    return JS_UNDEFINED;   /* getCurrentPosition returns void */
}
static JSValue geo_watch(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; drive_success(ctx, argc, argv);
    return js_concolic(ctx, "{watchId}", JS_UNDEFINED);   /* watchPosition returns a long watch id */
}

JSValue geolocation_make(JSContext *ctx) {
    JSValue g = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, g, "getCurrentPosition", JS_NewCFunction(ctx, geo_get_current, "getCurrentPosition", 1));
    JS_SetPropertyStr(ctx, g, "watchPosition", JS_NewCFunction(ctx, geo_watch, "watchPosition", 1));
    JS_SetPropertyStr(ctx, g, "clearWatch", JS_NewCFunction(ctx, js_noop, "clearWatch", 1));   /* no scriptable result */
    return idl_dfail_wrap(ctx, g, "Geolocation");
}
