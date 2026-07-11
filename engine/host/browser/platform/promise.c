/* Promise construction helpers — see promise.h. Moved from main.c: pure JS-engine edges, no scheduler state. */
#include "platform/promise.h"

/* wrap val in an already-RESOLVED promise (consumes val) so `await`/`.then` chains continue synchronously. */
JSValue js_resolved(JSContext *ctx, JSValue val)
{
    JSValue rf[2]; JSValue promise = JS_NewPromiseCapability(ctx, rf);
    if (!JS_IsException(promise)) {
        JSValue rr = JS_Call(ctx, rf[0], JS_UNDEFINED, 1, &val); JS_FreeValue(ctx, rr);
        JS_FreeValue(ctx, rf[0]); JS_FreeValue(ctx, rf[1]);
    }
    JS_FreeValue(ctx, val);
    return promise;
}
/* A REJECTED promise (CONSUMES err): the await re-throws err into the continuation so try/catch/.catch runs —
   e.g. Response.json() on a malformed body rejects, never resolves to a fake concolic that hides the throw path. */
JSValue js_rejected(JSContext *ctx, JSValue err)
{
    JSValue rf[2]; JSValue promise = JS_NewPromiseCapability(ctx, rf);
    if (!JS_IsException(promise)) {
        JSValue rr = JS_Call(ctx, rf[1], JS_UNDEFINED, 1, &err); JS_FreeValue(ctx, rr);
        JS_FreeValue(ctx, rf[0]); JS_FreeValue(ctx, rf[1]);
    }
    JS_FreeValue(ctx, err);
    return promise;
}
