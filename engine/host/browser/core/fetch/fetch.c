/* THE FETCH API — Blink core/fetch. `fetch(input, init)` per the Fetch IDL: the request's URL comes from
 * `input` (a USVString or a Request-shaped object's `url`), its method from `init.method`, defaulting to GET.
 *
 * WHAT THIS COMPONENT IS FOR is the tool's headline output: every request the forced execution reaches funnels
 * one endpoint into the @H surface, whether or not it ever fires. A concolic URL contributes its SHAPE, a
 * concrete one its literal, and endpoint_record owns both — this file reads the IDL's arguments and nothing
 * else.
 *
 * IT DOES NOT FETCH. SECURITY.md puts every byte of network through the trusted `safeFetch` chokepoint, which
 * the untrusted WASM cannot reach; a body arrives later through the host's reply provision, and a flow that
 * awaits one parks (engine_pending_fetch). So the promise here RESOLVES EMPTY rather than pending: parking a
 * flow on a reply that this build has no way to deliver would hang it, and a hang is not the honest shape of a
 * missing capability — the missing capability is reply provision, and it names itself at qjs_provide. */
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "solver/endpoint.h"
#include "core/fetch/fetch.h"

/* 5.4 Request: `input` is a USVString OR a Request, whose URL is its `url` attribute.
   THAT READ CANNOT HAPPEN HERE. It is an ordinary [[Get]] on an object the page supplies, so it is one accessor
   or Proxy trap away from being the page's code — and running page code from a C builtin with no flow base is
   what the twelve internal-method entries abort on. Reading it with JS_GetPropertyStr aborted on
   `fetch(new Proxy({url:"/q"}, {get(){}}))` the first time this component ran, exactly as designed.
   The read has to be a REQUEST, which means `fetch` has to be a step machine (JS_CFUNC_STEP_DEF +
   step_getprop_run) — and it cannot be one yet, because the STEPDEF registry is a table inside quickjs.c with
   no host-facing registration. That door is the browser half's keystone; until it exists, this component
   handles the USVString form and refuses the Request form at its own name rather than reading it unsafely. */
static JSValue fetch_request_url(JSContext *ctx, JSValueConst input)
{
    if (JS_IsObject(input))
        DFAIL("fetch was handed a Request — reading its `url` is a [[Get]] on the page's object and belongs in "
              "a step machine; export a host-facing step-def registration from the fork so a browser component "
              "can declare one, then issue this read through step_getprop_run");
    return JS_DupValue(ctx, input);   /* a USVString, or a value ToString names an endpoint from */
}

static JSValue js_fetch(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    const char *method = "GET", *mc = NULL;
    JSValue url, promise, resolving[2];

    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "fetch requires at least 1 argument");

    /* `init.method` is the same problem one argument over: a [[Get]] on a page-supplied bag. Same answer. */
    if (argc > 1 && JS_IsObject(argv[1]))
        DFAIL("fetch was handed an init bag — reading its `method` is a [[Get]] on the page's object and "
              "belongs in a step machine; see fetch_request_url");

    url = fetch_request_url(ctx, argv[0]);
    if (JS_IsException(url)) { if (mc) JS_FreeCString(ctx, mc); return url; }
    endpoint_record(ctx, method, url);
    JS_FreeValue(ctx, url);
    if (mc) JS_FreeCString(ctx, mc);

    /* The IDL says a Promise<Response>. There is no Response until the host provides a body, so this settles
       with undefined: `await fetch(u)` continues, and whatever the page reads off the result is absent rather
       than wrong. Building a shape-only Response with noop methods here is the stub the IDL audit exists to
       expose — the real one belongs in core/fetch/response.c once bodies arrive. */
    promise = JS_NewPromiseCapability(ctx, resolving);
    if (JS_IsException(promise)) return promise;
    JSValue r = JS_Call(ctx, resolving[0], JS_UNDEFINED, 0, NULL);
    JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, resolving[0]);
    JS_FreeValue(ctx, resolving[1]);
    return promise;
}

void fetch_install(JSContext *ctx, JSValueConst global)
{
    DCHECK(JS_IsObject(global), "the fetch install was handed something that is not the global object");
    JS_SetPropertyStr(ctx, (JSValue)global, "fetch",
                      JS_NewCFunction(ctx, js_fetch, "fetch", 1));
}
