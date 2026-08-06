/* THE FETCH API — Blink core/fetch, as a STEP MACHINE.
 *
 * `fetch(input, init)`: the request's URL comes from `input` (a USVString, or a Request whose `url` attribute
 * names it) and its method from `init.method`, defaulting to GET. Every request forced execution reaches
 * funnels one endpoint into the @H surface, whether or not it ever fires — that is the tool's headline output,
 * and endpoint_record owns the shape/literal decision.
 *
 * IT IS A STEP MACHINE BECAUSE ITS ARGUMENTS ARE THE PAGE'S OBJECTS. Both IDL reads are ordinary [[Get]]s on
 * values the page supplied, so each is one accessor or Proxy trap away from being the page's code — and running
 * that from a C builtin with no flow base is what the engine's twelve internal-method entries abort on. The
 * first version of this file read them with JS_GetPropertyStr and aborted on
 * `fetch(new Proxy({url:"/q"}, {get(){}}))` immediately. Each read is a REQUEST now: the machine parks, the
 * trap runs on the tramp where it can suspend and fork, and the machine is re-entered with the answer.
 *
 * IT DOES NOT FETCH. SECURITY.md puts every byte of network behind the trusted safeFetch chokepoint the sandbox
 * cannot reach, so the promise is PENDING and the URL goes on the flow's own register: the trusted host fetches
 * it, qjs_provide fills the entry, and the flow's continuation resumes with the body. The flow cannot finish
 * while a reply is owed, which is what keeps reply-gated code reachable. */
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "solver/concolic.h"
#include "quickjs.h"
#include "quickjs-step.h"
#include "solver/endpoint.h"
#include "solver/engine.h"
#include "core/fetch/fetch.h"
#include "core/fetch/response.h"

/* The id JS_RegisterStepDef handed this runtime, and the two IDL attribute names the machine reads by. Atoms
   belong to a runtime, and SECURITY.md gives this build one WASM instance per DOCUMENT — one runtime — so these
   are that runtime's. The DCHECK at install is what makes a second one impossible rather than merely unlikely. */
static int    g_fetch_stepid = -1;
static JSAtom g_atom_method, g_atom_url;
static JSRuntime *g_fetch_rt;

typedef struct JSFetchState {
    JSStepHdr hdr;      /* FIRST — the driver writes the def and the operand bounds through it */
    uint8_t   stage;
    JSValue   method;   /* the answer to the `init.method` read, or undefined */
    JSValue   url;      /* the answer to the `input.url` read, or the USVString input itself */
} JSFetchState;

/* WHAT THIS MACHINE OWNS. Declared once and read by both consumers: the teardown releases each, and the
   deep-fork clone takes a second reference — a concolic branch inside the `url` getter forks the flow at that
   depth, and the two arms must not share one answer slot. */
static void js_fetch_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSFetchState *s = st;
    v->val(ctx, &s->method);
    v->val(ctx, &s->url);
}

/* Park the request on the FLOW that issued it: the URL the trusted host must fetch, and the capability that
   delivers the body into this flow's continuation. There is ONE pending register and it is the flow's own —
   a second one beside it cannot resolve into the flow that owns the reaction, which is the whole point. */
/* THE REPLY BECOMES A RESPONSE HERE, in the component that promised one. The flow's pending register delivers
   the host's bytes and knows nothing about the Fetch API; this closure sits between them, so `fetch()` keeps its
   contract (a Promise<Response>) without the scheduler learning what a Response is. func_data = [resolve, url]. */
static JSValue fetch_deliver(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                             int magic, JSValueConst *func_data)
{
    const char *u = JS_ToCString(ctx, func_data[1]);
    /* WITH ITS LENGTH: a reply is a byte sequence, and the interior NUL a strlen stops at is exactly what
       `arrayBuffer()` would then have under-reported. */
    size_t body_len = 0;
    const char *body = argc > 0 ? JS_ToCStringLen(ctx, &body_len, argv[0]) : NULL;
    JSValue resp, r;

    (void)this_val; (void)magic;
    resp = response_new(ctx, u ? u : "", body ? body : "", body ? body_len : 0);
    if (u) JS_FreeCString(ctx, u);
    if (body) JS_FreeCString(ctx, body);
    if (JS_IsException(resp))
        return resp;
    r = JS_Call(ctx, func_data[0], JS_UNDEFINED, 1, (JSValueConst *)&resp);
    JS_FreeValue(ctx, resp);
    return r;
}

static JSValue fetch_park(JSContext *ctx, JSValueConst url)
{
    JSValue promise, resolving[2], deliver;
    JSValueConst data[2];
    const char *u;

    promise = JS_NewPromiseCapability(ctx, resolving);
    if (JS_IsException(promise))
        return promise;
    /* THE CONCRETE PROJECTION IS ASKED FOR EXPLICITLY. A URL built out of unknown external input is a CONCOLIC,
       and the ToString boundary owes C a real string rather than one — so this edge reads the concolic's own
       SHAPE, which is the display form the @H surface reports ("/api/{hash}"). Coercing it generically would
       either crash at that boundary or, worse, quietly de-taint the URL. */
    if (concolic_is(url)) {
        /* the SHAPE, made a real string first, so there is exactly ONE ownership rule for `u` below. */
        const char *sh = concolic_shape_c(url);
        JSValue sv = JS_NewString(ctx, sh ? sh : "{}");
        u = JS_ToCString(ctx, sv);
        JS_FreeValue(ctx, sv);
    } else {
        u = JS_ToCString(ctx, url);
    }
    if (u) {
        JSValue uv = JS_NewString(ctx, u);
        data[0] = resolving[0];
        data[1] = uv;
        deliver = JS_NewCFunctionData(ctx, fetch_deliver, 1, 0, 2, data);
        JS_FreeValue(ctx, uv);
        if (!JS_IsException(deliver)) {
            engine_pending_fetch_url(ctx, deliver, JS_UNDEFINED, u);
            JS_FreeValue(ctx, deliver);
        }
        JS_FreeCString(ctx, u);
    }
    JS_FreeValue(ctx, resolving[0]);
    JS_FreeValue(ctx, resolving[1]);
    return promise;
}

static JSValue js_fetch_fini(JSContext *ctx, void *st, bool take_result)
{
    JSFetchState *s = st;
    JSValue promise = JS_UNDEFINED;

    if (take_result)
        promise = fetch_park(ctx, s->url);
    JS_FreeValue(ctx, s->method);
    JS_FreeValue(ctx, s->url);
    return promise;
}

/* Stage 0 reads `init.method`, stage 1 reads `input.url`, stage 2 records the endpoint. Each read is a request
   the machine parks on; an argument that needs no read falls straight through to the next stage. */
static int js_fetch_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSFetchState *s = st;
    JSValueConst input = step_arg(&s->hdr, 0), init = step_arg(&s->hdr, 1);
    const char *method = "GET", *mc = NULL;
    int r;

    if (s->stage == 0) {
        if (JS_IsObject(init)) {
            r = step_getprop_run(ctx, &s->hdr, init, g_atom_method, cb_result, &s->method, out_cb, out_argc);
            if (r > 0) return r;
            if (r < 0) return JS_STEP_ABRUPT;
        } else {
            JS_FreeValue(ctx, cb_result);
        }
        cb_result = JS_UNDEFINED;
        s->stage = 1;
    }
    if (s->stage == 1) {
        /* 5.4 Request: a USVString names the URL directly; a Request names it through its `url` attribute, and
           THAT is the read that has to be a request. */
        if (JS_IsObject(input)) {
            r = step_getprop_run(ctx, &s->hdr, input, g_atom_url, cb_result, &s->url, out_cb, out_argc);
            if (r > 0) return r;
            if (r < 0) return JS_STEP_ABRUPT;
        } else {
            JS_FreeValue(ctx, cb_result);
            s->url = JS_DupValue(ctx, input);
        }
        s->stage = 2;
    }

    DCHECK(s->stage == 2, "the fetch machine was re-entered at a stage it never parks in");
    if (JS_IsString(s->method)) {
        mc = JS_ToCString(ctx, s->method);
        if (mc) method = mc;
    }
    endpoint_record(ctx, method, s->url);
    if (mc) JS_FreeCString(ctx, mc);
    return JS_STEP_DONE;
}

static const JSTrampStepDef js_fetch_def = {
    sizeof(JSFetchState), js_fetch_step, js_fetch_fini, 0, .visit = js_fetch_visit
};

void fetch_install(JSContext *ctx, JSValueConst global)
{
    JSRuntime *rt = JS_GetRuntime(ctx);

    DCHECK(JS_IsObject(global), "the fetch install was handed something that is not the global object");
    DCHECK(g_fetch_rt == NULL || g_fetch_rt == rt,
           "fetch was installed into a second runtime — its atoms and step id belong to the first, and one WASM "
           "instance is one document");
    if (g_fetch_stepid < 0) {
        g_fetch_rt    = rt;
        response_init(ctx);
        g_atom_method = JS_NewAtom(ctx, "method");
        g_atom_url    = JS_NewAtom(ctx, "url");
        g_fetch_stepid = JS_RegisterStepDef(rt, &js_fetch_def);
    }
    JS_SetPropertyStr(ctx, (JSValue)global, "fetch",
                      JS_NewCFunction2(ctx, NULL, "fetch", 1, JS_CFUNC_step, g_fetch_stepid));
}
