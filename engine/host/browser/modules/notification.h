/* Notification — Blink modules/notifications. `new Notification(title, opts)` + the static
 * Notification.requestPermission(). See notification.c. */
#ifndef ENGINE_HOST_BROWSER_NOTIFICATION_H
#define ENGINE_HOST_BROWSER_NOTIFICATION_H
#include "quickjs.h"
JSValue js_notification_ctor(JSContext *ctx, JSValueConst nt, int argc, JSValueConst *argv);   /* new Notification(...) */
JSValue js_notif_request_perm(JSContext *ctx, JSValueConst t, int c, JSValueConst *v);         /* Notification.requestPermission() */
#endif
