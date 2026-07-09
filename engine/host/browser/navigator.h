/* Network-initiating navigator methods — Blink core/frame/navigator + modules/service_worker.
 * sendBeacon(url,data) is a real POST (telemetry endpoint); serviceWorker.register(url) fetches + analyzes the
 * SW script like a chunk. See navigator.c. */
#ifndef ENGINE_HOST_BROWSER_NAVIGATOR_H
#define ENGINE_HOST_BROWSER_NAVIGATOR_H
#include "quickjs.h"
JSValue js_sw_register(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);   /* navigator.serviceWorker.register(url) */
JSValue js_send_beacon(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);   /* navigator.sendBeacon(url, data) */
#endif
