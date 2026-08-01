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
 * cannot reach, so the promise settles rather than pending: parking a flow on a reply this build has no way to
 * deliver would hang it, and a hang is not the honest shape of a missing capability — the missing capability is
 * reply provision, and it names itself at qjs_provide. */
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "quickjs-step.h"
#include "solver/endpoint.h"
#include "solver/engine.h"
#include "core/fetch/fetch.h"

/* The id JS_RegisterStepDef handed this runtime, and the two IDL attribute names the machine reads by. Atoms
   belong to a runtime, and SECURITY.md gives this build one WASM instance per DOCUMENT — one runtime — so these
   are that runtime's. The DCHECK at install is what makes a second one impossible rather than merely unlikely. */
static int    g_fetch_stepid = -1;
static JSAtom g_atom_method, g_atom_url;
static JSRuntime *g_fetch_rt;

/* THE PENDING REGISTER. One entry per fetch whose promise has not been resolved: the URL the host must fetch
   and the resolve capability that delivers the body into the flow parked on it. It grows and never compacts
   below its high-water mark — the entries are the frontier's, and a bound on them would be a bound on how many
   requests a page may have in flight, which is a cap. */
typedef struct FetchPending {
    char   *url;        /* owned */
    JSValue resolve;    /* owned; JS_UNDEFINED once delivered */
    int     is_script;  /* the body is JS to RUN in this flow, not data to hand back */
} FetchPending;

static FetchPending *g_pending;
static int           g_pending_count, g_pending_size;
static char         *g_pending_join;   /* the newline-joined answer fetch_pending_urls last built */

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

/* Park a request: record the URL the host must fetch and the capability that delivers its body. */
static JSValue fetch_park(JSContext *ctx, JSValueConst url)
{
    JSValue promise, resolving[2];
    const char *u;
    FetchPending *p;

    promise = JS_NewPromiseCapability(ctx, resolving);
    if (JS_IsException(promise))
        return promise;
    u = JS_ToCString(ctx, url);
    if (!u) { JS_FreeValue(ctx, resolving[0]); JS_FreeValue(ctx, resolving[1]); return promise; }

    if (g_pending_count == g_pending_size) {
        int n = g_pending_size ? g_pending_size * 2 : 8;
        FetchPending *a = realloc(g_pending, sizeof(*a) * (size_t)n);
        CHECK(a != NULL, "the pending-fetch register allocation failed: a dropped request loses the flow "
                         "parked on it, and the frontier cannot tell that from a request nobody made");
        g_pending = a;
        g_pending_size = n;
    }
    p = &g_pending[g_pending_count++];
    p->url = strdup(u);
    CHECK(p->url != NULL, "the pending-fetch URL allocation failed");
    p->resolve = JS_DupValue(ctx, resolving[0]);
    /* A body whose URL the page loaded as a SCRIPT is more code this flow runs; anything else is data. The
       decision is the caller's edge (loadScript vs fetch), recorded here so provision does not have to guess. */
    p->is_script = 0;
    JS_FreeCString(ctx, u);
    JS_FreeValue(ctx, resolving[0]);
    JS_FreeValue(ctx, resolving[1]);
    return promise;
}

const char *fetch_pending_urls(JSContext *ctx)
{
    size_t need = 1;
    int i;

    (void)ctx;
    free(g_pending_join);
    g_pending_join = NULL;
    for (i = 0; i < g_pending_count; i++)
        if (!JS_IsUndefined(g_pending[i].resolve))
            need += strlen(g_pending[i].url) + 1;
    g_pending_join = malloc(need);
    CHECK(g_pending_join != NULL, "the pending-URL join allocation failed");
    g_pending_join[0] = '\0';
    for (i = 0; i < g_pending_count; i++) {
        if (JS_IsUndefined(g_pending[i].resolve)) continue;
        strcat(g_pending_join, g_pending[i].url);
        strcat(g_pending_join, "\n");
    }
    return g_pending_join;
}

int fetch_provide(JSContext *ctx, const char *url, const char *body, int is_script)
{
    int i, n = 0;

    DCHECK(url != NULL, "a body was provided for no URL");
    for (i = 0; i < g_pending_count; i++) {
        FetchPending *p = &g_pending[i];
        if (JS_IsUndefined(p->resolve) || strcmp(p->url, url) != 0)
            continue;
        if (is_script || p->is_script) {
            /* Code-loading async ALWAYS executes: the body is more of THIS flow's program, discovered behind
               whatever branch reached the load, and it forks through the same one BFS. */
            engine_queue_script(body ? body : "");
        }
        /* RESOLVE DIRECTLY, not through engine_pending_fetch. That one is for a fetch issued from INSIDE a
           running flow — it asserts a live flow, and provision happens BETWEEN quanta, with none running. The
           capability is the engine's own resolving function, so calling it runs no page code; it enqueues the
           reaction, which is a first-class flow the next qjs_step schedules like any other.
           The value is the body TEXT, not a Response: a Response is a real object with a real body-reading
           state machine and belongs in core/fetch/response.c — a shape-only one with noop methods is the stub
           the IDL audit exists to expose. */
        {
            JSValue arg = body ? JS_NewString(ctx, body) : JS_UNDEFINED;
            JSValue r = JS_Call(ctx, p->resolve, JS_UNDEFINED, 1, (JSValueConst *)&arg);
            DCHECK(!JS_IsException(r), "resolving a parked fetch threw — the capability is the engine's own");
            JS_FreeValue(ctx, r);
            JS_FreeValue(ctx, arg);
        }
        JS_FreeValue(ctx, p->resolve);
        p->resolve = JS_UNDEFINED;
        n++;
    }
    return n;
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
        g_atom_method = JS_NewAtom(ctx, "method");
        g_atom_url    = JS_NewAtom(ctx, "url");
        g_fetch_stepid = JS_RegisterStepDef(rt, &js_fetch_def);
    }
    JS_SetPropertyStr(ctx, (JSValue)global, "fetch",
                      JS_NewCFunction2(ctx, NULL, "fetch", 1, JS_CFUNC_step, g_fetch_stepid));
}
