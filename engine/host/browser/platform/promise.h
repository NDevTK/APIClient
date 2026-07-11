#ifndef ENGINE_HOST_BROWSER_PLATFORM_PROMISE_H
#define ENGINE_HOST_BROWSER_PLATFORM_PROMISE_H
#include "quickjs.h"
/* Promise construction helpers (platform/JS-engine edge): a host-edge that models an async web API returns one
 * of these so an `await`/`.then` chain continues in the SAME flow. Extracted from main.c — every DOM/loader
 * component (blob/navigator/media/reply/...) returned these via a private `extern`; one header replaces them. */

JSValue js_resolved(JSContext *ctx, JSValue val);   /* an already-RESOLVED promise wrapping val (consumes val) — await continues synchronously */
JSValue js_rejected(JSContext *ctx, JSValue err);   /* a REJECTED promise (consumes err) — await RE-THROWS err into the continuation (try/catch/.catch runs) */

#endif
