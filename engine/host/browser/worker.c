/* Worker + SharedWorker — see worker.h. Extracted from main.c. The worker SCRIPT is code with its OWN
 * endpoints, so fetch + analyze it like a <script src> chunk (chunk_pending_add); the object exposes
 * postMessage/terminate + addEventListener (onmessage is a scheduler FLOW) and a .port (SharedWorker). */
#include <stdlib.h>
#include "worker.h"
#include "url.h"        /* url_from_arg, has_hole */
#include "opaque.h"     /* js_noop */

extern JSValue js_add_listener(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv);   /* scheduler-side (main.c) */
extern void chunk_pending_add(const char *url);                                                 /* scheduler-side: queue a script chunk for host fetch + analyze */

JSValue js_worker_ctor(JSContext *ctx, JSValueConst nt, int argc, JSValueConst *argv) {
    if (argc >= 1) {
        char *url = url_from_arg(ctx, argv[0]);
        if (url) { if (!has_hole(url)) chunk_pending_add(url); free(url); }   /* -> host fetch + engine analyze */
    }
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "postMessage", JS_NewCFunction(ctx, js_noop, "postMessage", 1));
    JS_SetPropertyStr(ctx, o, "terminate", JS_NewCFunction(ctx, js_noop, "terminate", 0));
    JS_SetPropertyStr(ctx, o, "addEventListener", JS_NewCFunction(ctx, js_add_listener, "addEventListener", 2));
    JS_SetPropertyStr(ctx, o, "removeEventListener", JS_NewCFunction(ctx, js_noop, "removeEventListener", 2));
    JSValue port = JS_NewObject(ctx);   /* SharedWorker.port (MessagePort) */
    JS_SetPropertyStr(ctx, port, "postMessage", JS_NewCFunction(ctx, js_noop, "postMessage", 1));
    JS_SetPropertyStr(ctx, port, "start", JS_NewCFunction(ctx, js_noop, "start", 0));
    JS_SetPropertyStr(ctx, port, "close", JS_NewCFunction(ctx, js_noop, "close", 0));
    JS_SetPropertyStr(ctx, port, "addEventListener", JS_NewCFunction(ctx, js_add_listener, "addEventListener", 2));
    JS_SetPropertyStr(ctx, o, "port", port);
    return o;
}
