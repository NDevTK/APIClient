/* Response — Blink core/fetch/Response, the PAGE-CONSTRUCTED `new Response(body, init)` (a service worker's
 * respondWith, a test double, `new Response(JSON.stringify(data))`). Distinct from a FETCH response (whose body
 * comes from the server reply via reply.c/make_response): here the body is the constructor argument, so
 * .text()/.json()/.blob() carry THAT value's taint — exactly like Blob. See response.c. */
#ifndef ENGINE_HOST_BROWSER_RESPONSE_H
#define ENGINE_HOST_BROWSER_RESPONSE_H
#include "quickjs.h"
void response_init(JSContext *ctx);                                                       /* register the Response class (qjs_init) */
JSValue js_response_ctor(JSContext *ctx, JSValueConst nt, int argc, JSValueConst *argv);   /* new Response(body, init) */
#endif
