/* Network Information module — see network_information.h. */
#include "modules/netinfo/network_information.h"
#include "bindings/idl.h"        /* idl_dfail_wrap — unbuilt NetworkInformation members DFAIL */
#include "solver/concolic.h"     /* js_concolic, js_noop */

extern JSValue js_add_listener(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv);   /* 'change' -> driven flow */

JSValue network_information_make(JSContext *ctx) {
    JSValue c = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, c, "effectiveType", js_concolic(ctx, "{effectiveType}", JS_NewString(ctx, "4g")));   /* forks 4g/3g/2g/slow-2g adaptive arms */
    JS_SetPropertyStr(ctx, c, "type", js_concolic(ctx, "{connectionType}", JS_NewString(ctx, "wifi")));
    JS_SetPropertyStr(ctx, c, "downlink", js_concolic(ctx, "{downlink}", JS_NewFloat64(ctx, 10.0)));            /* Mbps */
    JS_SetPropertyStr(ctx, c, "rtt", js_concolic(ctx, "{rtt}", JS_NewInt32(ctx, 50)));                          /* ms */
    JS_SetPropertyStr(ctx, c, "saveData", js_concolic(ctx, "{saveData}", JS_FALSE));                            /* forks the data-saver code path */
    JS_SetPropertyStr(ctx, c, "onchange", JS_NULL);
    JS_SetPropertyStr(ctx, c, "addEventListener", JS_NewCFunction(ctx, js_add_listener, "addEventListener", 2));  /* the 'change' listener becomes an orphan flow */
    JS_SetPropertyStr(ctx, c, "removeEventListener", JS_NewCFunction(ctx, js_noop, "removeEventListener", 2));
    return idl_dfail_wrap(ctx, c, "NetworkInformation");   /* unbuilt members DFAIL, never an opaque shrug */
}
