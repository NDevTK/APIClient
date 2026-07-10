/* Notification — see notification.h. Built from its Web IDL via the idl.h driver:
 *
 *   interface Notification : EventTarget { undefined close(); ... addEventListener/removeEventListener };
 *
 * requestPermission() resolves to a permission the user grants — genuinely unknown headless, so it reads as
 * the opaque concolic value (a gate `if (perm === 'granted')` FORKS, reaching the permission-gated code). */
#include "modules/notification.h"
#include "bindings/idl.h"
#include "opaque.h"   /* g_opaque, js_noop */

extern JSValue js_add_listener(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv);

static const IDLMember NOTIFICATION_IDL[] = {
    { "close",               IDL_METHOD, js_noop,         0 },
    { "addEventListener",    IDL_METHOD, js_add_listener, 2 },
    { "removeEventListener", IDL_METHOD, js_noop,         2 },
};

JSValue js_notification_ctor(JSContext *ctx, JSValueConst nt, int argc, JSValueConst *argv) {
    (void)nt; (void)argc; (void)argv;
    return idl_instance(ctx, NOTIFICATION_IDL, sizeof NOTIFICATION_IDL / sizeof NOTIFICATION_IDL[0]);
}
JSValue js_notif_request_perm(JSContext *ctx, JSValueConst t, int c, JSValueConst *v) {
    (void)t; (void)c; (void)v;
    return js_concolic(ctx, "{notificationPermission}", JS_UNDEFINED);   /* Promise-awaited permission is unknown -> forks the granted/denied gate */
}
