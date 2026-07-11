/* Credential Management module — see credentials.h. */
#include "modules/credentialmanagement/credentials.h"
#include "bindings/idl.h"        /* idl_dfail_wrap — unbuilt CredentialsContainer members DFAIL */
#include "solver/concolic.h"     /* js_concolic — the concolic Credential */

#include "platform/promise.h"

static JSValue cred_promise_undef(JSContext *ctx, JSValueConst t, int c, JSValueConst *v) { (void)t; (void)c; (void)v; return js_resolved(ctx, JS_UNDEFINED); }

/* get()/store()/create(): resolve to a CONCOLIC Credential — TRUTHY example so the logged-in arm runs, concolic
   so the null (logged-out) arm still forks. This is the moat's core: the admin/session code behind the auth gate. */
static JSValue cred_get(JSContext *ctx, JSValueConst t, int c, JSValueConst *v) {
    (void)t; (void)c; (void)v;
    return js_resolved(ctx, js_concolic(ctx, "{credential}", JS_TRUE));   /* Promise<Credential|null> */
}

JSValue credentials_make(JSContext *ctx) {
    JSValue cc = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, cc, "get", JS_NewCFunction(ctx, cred_get, "get", 1));
    JS_SetPropertyStr(ctx, cc, "store", JS_NewCFunction(ctx, cred_get, "store", 1));
    JS_SetPropertyStr(ctx, cc, "create", JS_NewCFunction(ctx, cred_get, "create", 1));
    JS_SetPropertyStr(ctx, cc, "preventSilentAccess", JS_NewCFunction(ctx, cred_promise_undef, "preventSilentAccess", 0));
    return idl_dfail_wrap(ctx, cc, "CredentialsContainer");   /* unbuilt members DFAIL */
}
