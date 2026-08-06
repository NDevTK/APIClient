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
#include "core/fetch/fetch.h"
#include "core/file/blob.h"
#include "core/frame/location.h"
#include "core/fetch/headers.h"
#include "core/fetch/body.h"
#include "core/fetch/response.h"
#include "core/fetch/request.h"

/* The id JS_RegisterStepDef handed this runtime, and the two IDL attribute names the machine reads by. Atoms
   belong to a runtime, and SECURITY.md gives this build one WASM instance per DOCUMENT — one runtime — so these
   are that runtime's. The DCHECK at install is what makes a second one impossible rather than merely unlikely. */
static const FetchProvider *g_provider;

/* §4.4's URL parse, against HTML's API base URL. Both of Fetch's entry points that take a URL STRING come
   here, so a relative endpoint — which is how a bundle names its own API — resolves the same way from both.
   A document with no address has no base, and the parse is then absolute-only: that is the honest answer for a
   platform-less build rather than a base this component invented. */
bool fetch_parse_url(UrlRecord *rec, const char *url, size_t len)
{
    const char *base_str = location_api_base_url();
    UrlRecord base;
    bool ok;

    if (!base_str)
        return url_parse(rec, url, len, NULL);
    url_record_init(&base);
    ok = url_parse(&base, base_str, strlen(base_str), NULL) && url_parse(rec, url, len, &base);
    url_record_free(&base);
    return ok;
}

void fetch_set_provider(const FetchProvider *p) { g_provider = p; }

static int    g_fetch_stepid = -1;
static JSAtom g_atom_method, g_atom_url, g_atom_headers, g_atom_body;
static JSRuntime *g_fetch_rt;

typedef struct JSFetchState {
    JSStepHdr hdr;      /* FIRST — the driver writes the def and the operand bounds through it */
    uint8_t   stage;
    JSValue   method;   /* the answer to the `init.method` read, or undefined */
    JSValue   url;      /* the answer to the `input.url` read, or the USVString input itself */
    JSValue   input;    /* the argument ITSELF — a Request carries a captured blob URL entry the URL cannot */
    JSValue   hinit;    /* the answer to the `init.headers` read — a HeadersInit the fill converts */
    JSValue   binit;    /* the answer to the `init.body` read — a BodyInit §5.1's extraction turns into bytes */
    BodyState body;     /* those bytes. A POST that dropped its body asked the server a different question */
    /* §5.1's Content-Type for that body, as a JS STRING rather than a malloc'd one: a step state is BYTE-COPIED
       at a deep fork and only what `visit` names is re-taken, so a heap pointer here would be freed by both
       arms. JS_UNDEFINED for a body whose arm has no type. */
    JSValue   body_mime;
    HeadersFill fill;   /* the fill's cursor: it parks per key, so it cannot be a loop here */
    HeaderList  hdrs;   /* what the request carries, which is half of what makes the endpoint usable */
} JSFetchState;

/* WHAT THIS MACHINE OWNS. Declared once and read by both consumers: the teardown releases each, and the
   deep-fork clone takes a second reference — a concolic branch inside the `url` getter forks the flow at that
   depth, and the two arms must not share one answer slot. */
static void js_fetch_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSFetchState *s = st;
    v->val(ctx, &s->method);
    v->val(ctx, &s->url);
    v->val(ctx, &s->input);
    v->val(ctx, &s->hinit);
    v->val(ctx, &s->binit);
    v->val(ctx, &s->body_mime);
    headers_fill_visit(ctx, &s->fill, v);   /* the fill's own slots — it parks mid-conversion like any machine */
}

/* Park the request on the FLOW that issued it: the URL the trusted host must fetch, and the capability that
   delivers the body into this flow's continuation. There is ONE pending register and it is the flow's own —
   a second one beside it cannot resolve into the flow that owns the reaction, which is the whole point. */
/* THE REPLY BECOMES A RESPONSE HERE, in the component that promised one. The host delivers bytes and knows
   nothing about the Fetch API; this closure sits between them, so `fetch()` keeps its contract (a
   Promise<Response>) without the host learning what a Response is. func_data = [resolve, url, reject].
   A NULL delivery is §5.5's NETWORK ERROR and REJECTS with a TypeError. It used to be stringified into the
   body like any other value, so a request the host could not satisfy resolved with a Response reading "null"
   — a page's `.catch` never ran, and the failure arrived disguised as data. */
/* THE REPLY'S DELIVERY IS A STEP MACHINE, because settling the promise is the PAGE'S code. 27.2.1.3.2 step 8
 * reads `Get(resolution, "then")` off the value being resolved with, and the value here is a Response — an
 * ordinary object whose prototype the page owns, so `Object.prototype.then = { get(){…} }` makes that read the
 * page's, and prototype pollution is a gadget class this engine exists to RUN rather than assume away. It was a
 * JS_Call out of the host's pump: an activation with no flow base, where a loop in that getter drives to
 * completion. body.c fixed exactly this for its readers; this is the same defect one layer out.
 *
 * It is a CLOSURE machine — JS_NewStepClosure — because a reaction knows which promise it belongs to only by
 * capture, and the work it does (settling) is work only a machine may do. Those two were mutually exclusive
 * before: JS_NewCFunctionData gives capture without the machine, JS_CFUNC_step gives the machine without
 * capture. What it captured is read back off the callee its own header carries. */
enum { FETCH_DELIVER_RESOLVE = 0, FETCH_DELIVER_URL, FETCH_DELIVER_REJECT };

typedef struct {
    JSStepHdr hdr;      /* FIRST — the driver writes the def and the operand bounds through it */
    uint8_t   stage;
    uint8_t   cphase;   /* the settle call's own phase, held across a suspension */
    uint8_t   reject;   /* which of the capability's two functions this delivery settles with */
    JSValue   value;    /* the Response, or the TypeError a null delivery makes (owned) */
    JSValue   cb[3];    /* the call request buffer: [this, resolving function, value] */
} JSFetchDeliverState;

static void js_fetch_deliver_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSFetchDeliverState *s = st;
    int k;
    v->val(ctx, &s->value);
    for (k = 0; k < 3; k++) v->val(ctx, &s->cb[k]);
}

static JSValue js_fetch_deliver_fini(JSContext *ctx, void *st, bool take_result)
{
    JSFetchDeliverState *s = st;
    int k;
    (void)take_result;   /* a resolving function's return value is undefined and unobservable */
    JS_FreeValue(ctx, s->value);
    s->value = JS_UNDEFINED;
    for (k = 0; k < 3; k++) { JS_FreeValue(ctx, s->cb[k]); s->cb[k] = JS_UNDEFINED; }
    return JS_UNDEFINED;
}

static int js_fetch_deliver_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSFetchDeliverState *s = st;
    JSValue settled;
    int r;

    if (s->stage == 0) {
        JSValueConst body_v = s->hdr.argc > 0 ? s->hdr.argv[0] : JS_UNDEFINED;

        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        if (JS_IsNull(body_v)) {
            /* The host could not fetch it. §5.5 rejects with a TypeError, and the message is the one every
               browser uses, because a page that matches on it is matching on that. */
            JS_ThrowTypeError(ctx, "Failed to fetch");
            s->value = JS_GetException(ctx);
            s->reject = 1;
        } else {
            const char *u = JS_ToCString(ctx, JS_StepClosureData(&s->hdr, FETCH_DELIVER_URL));
            /* WITH ITS LENGTH: a reply is a byte sequence, and the interior NUL a strlen stops at is exactly
               what `arrayBuffer()` would then have under-reported. */
            size_t body_len = 0;
            const char *body = JS_ToCStringLen(ctx, &body_len, body_v);
            s->value = response_new(ctx, u ? u : "", body ? body : "", body ? body_len : 0, NULL);
            if (u) JS_FreeCString(ctx, u);
            if (body) JS_FreeCString(ctx, body);
            if (JS_IsException(s->value)) return JS_STEP_ABRUPT;
        }
        s->stage = 1;
    }

    DCHECK(s->stage == 1, "the fetch delivery was re-entered at a stage it never parks in");
    r = step_call_run(ctx, &s->cphase, s->cb,
                      JS_StepClosureData(&s->hdr, s->reject ? FETCH_DELIVER_REJECT : FETCH_DELIVER_RESOLVE),
                      JS_UNDEFINED, 1, (JSValueConst *)&s->value, cb_result, &settled, out_cb, out_argc);
    if (r > 0) return r;          /* parked ON THE SETTLE; the `then` read runs with a flow base under it */
    JS_FreeValue(ctx, settled);
    return JS_STEP_DONE;
}

static const JSTrampStepDef js_fetch_deliver_def = {
    sizeof(JSFetchDeliverState), js_fetch_deliver_step, js_fetch_deliver_fini, 0,
    .visit = js_fetch_deliver_visit
};
static int g_deliver_stepid = -1;

static JSValue fetch_park(JSContext *ctx, JSValueConst url, JSValueConst method, JSValueConst input,
                          const HeaderList *hdrs, const char *body, size_t body_len)
{
    JSValue promise, resolving[2], deliver;
    JSValueConst data[3];
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
    if (u && !strncmp(u, "blob:", 5)) {
        /* §5: a `blob:` URL is answered from the BLOB URL STORE, not from the network — the whole point of
           `URL.createObjectURL` is that the bytes are already here. The FRAGMENT is stripped before the lookup,
           because a fragment names a place within a resource and not a different entry.
           The settle goes through a FLOW like every other delivery: resolving reads `then` off the Response,
           which the page can own. */
        UrlRecord rec;
        char *key = fetch_parse_url(&rec, u, strlen(u)) ? url_serialize(&rec, true) : NULL;
        /* §5.3's CAPTURED ENTRY FIRST: a Request resolved its blob URL when it was built, so a page that
           revoked the URL afterwards still fetches. The store is consulted only for a URL STRING, which has
           nothing to have captured. */
        JSValueConst captured = request_blob_entry(input);
        JSValueConst blob = !JS_IsUndefined(captured) ? captured
                          : (key ? blob_url_lookup(key, strlen(key)) : JS_UNDEFINED);
        JSValue value;
        int reject = 0;
        /* §5: a blob fetch whose method is not GET is a NETWORK ERROR before the store is consulted. A blob URL
           names bytes that are already here; there is nothing for a POST or a DELETE to mean. */
        if (!JS_IsUndefined(method) && !JS_IsUndefined(blob)) {
            const char *m = JS_ToCString(ctx, method);
            if (m && strcmp(m, "GET")) blob = JS_UNDEFINED;
            if (m) JS_FreeCString(ctx, m);
        }

        url_record_free(&rec);
        free(key);
        if (JS_IsUndefined(blob)) {
            /* §5: no entry is a NETWORK ERROR, which `fetch` rejects with a TypeError. */
            JS_ThrowTypeError(ctx, "Failed to fetch");
            value = JS_GetException(ctx);
            reject = 1;
        } else {
            size_t blen = 0;
            const char *btype = NULL;
            const char *bytes = blob_bytes_of(blob, &blen, &btype);
            value = response_new(ctx, u, bytes, blen, btype);
        }
        if (!JS_IsException(value)) {
            if (JS_CallAsFlow(ctx, resolving[reject], value) < 0) {
                JSValue exc = JS_GetException(ctx);
                JS_FreeValue(ctx, exc);   /* the page's own handler threw; that is its completion, not this call's */
            }
            JS_FreeValue(ctx, value);
        }
        JS_FreeCString(ctx, u);
        JS_FreeValue(ctx, resolving[0]);
        JS_FreeValue(ctx, resolving[1]);
        return promise;
    }
    if (u) {
        JSValue uv = JS_NewString(ctx, u);
        data[0] = resolving[0];
        data[1] = uv;
        data[2] = resolving[1];
        DCHECK(g_deliver_stepid >= 0, "fetch() parked a reply before its delivery machine was declared");
        deliver = JS_NewStepClosure(ctx, g_deliver_stepid, 1, 3, data);
        JS_FreeValue(ctx, uv);
        if (!JS_IsException(deliver)) {
            DCHECK(g_provider != NULL && g_provider->owe != NULL,
                   "fetch() was called with no host network provider installed — the promise would be owed to "
                   "nobody and the flow could never finish");
            FetchRequest req;
            const char *m = JS_IsUndefined(method) ? NULL : JS_ToCString(ctx, method);
            req.method = m ? m : "GET";
            req.url = u;
            req.headers = hdrs;
            req.body = body;
            req.body_len = body_len;
            g_provider->owe(ctx, deliver, JS_UNDEFINED, &req);
            if (m) JS_FreeCString(ctx, m);
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
        if (!JS_IsUndefined(s->body_mime)) {
            char *have = header_list_get(&s->hdrs, "content-type");
            const char *m = have ? NULL : JS_ToCString(ctx, s->body_mime);
            if (m) { header_list_append(&s->hdrs, "content-type", m); JS_FreeCString(ctx, m); }
            free(have);
        }
        promise = fetch_park(ctx, s->url, s->method, s->input, &s->hdrs,
                             s->body.has ? s->body.bytes : NULL, s->body.len);
    JS_FreeValue(ctx, s->method);
    JS_FreeValue(ctx, s->url);
    JS_FreeValue(ctx, s->input);
    JS_FreeValue(ctx, s->hinit);
    JS_FreeValue(ctx, s->binit);
    body_state_free(ctx, &s->body);
    JS_FreeValue(ctx, s->body_mime);
    s->body_mime = JS_UNDEFINED;
    headers_fill_release(ctx, &s->fill);
    header_list_free(&s->hdrs);
    return promise;
}

/* Stage 0 reads `init.method`, stage 1 `input.url`, stage 2 `init.headers`, stage 3 converts that HeadersInit,
   and stage 4 records the endpoint. Each read is a request the machine parks on; an argument that needs no read
   falls straight through to the next stage. */
static int js_fetch_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSFetchState *s = st;
    JSValueConst input = step_arg(&s->hdr, 0), init = step_arg(&s->hdr, 1);
    const char *method = "GET", *mc = NULL;
    int r;

    if (s->stage == 0) {
        /* THE INPUT ITSELF, kept from the first entry. A Request carries a captured blob URL entry that its
           `url` string cannot express, and this is where every other answer slot is spelled out too. It cannot
           be deferred behind an "is it set yet" test on the slot: a zeroed step state's JSValue is the INTEGER
           0 and not JS_UNDEFINED, so such a test always reads as already-set. */
        s->input = JS_DupValue(ctx, input);
        if (JS_IsObject(init)) {
            r = step_getprop_run(ctx, &s->hdr, init, g_atom_method, cb_result, &s->method, out_cb, out_argc);
            if (r > 0) return r;
            if (r < 0) return JS_STEP_ABRUPT;
        } else {
            JS_FreeValue(ctx, cb_result);
            s->method = JS_UNDEFINED;   /* the skipped read's answer, spelled — see stage 2 */
        }
        cb_result = JS_UNDEFINED;
        s->stage = 5;
    }
    if (s->stage == 5) {
        /* 5.4 Request step 36: `init.body`. It was never read at all, so `fetch(u, {method:"POST", body:x})`
           asked the server a different question than the page did — and the endpoint this engine reports for it
           carried no body either. The READ is the page's code; the EXTRACTION afterwards is §5.1's, which
           body.c owns for every interface that takes a BodyInit. */
        if (JS_IsObject(init)) {
            r = step_getprop_run(ctx, &s->hdr, init, g_atom_body, cb_result, &s->binit, out_cb, out_argc);
            if (r > 0) return r;
            if (r < 0) return JS_STEP_ABRUPT;
        } else {
            JS_FreeValue(ctx, cb_result);
            s->binit = JS_UNDEFINED;
        }
        cb_result = JS_UNDEFINED;
        if (!JS_IsUndefined(s->binit) && !JS_IsNull(s->binit)) {
            char *mime = NULL;
            if (body_extract(ctx, &s->body, s->binit, &mime) < 0) { free(mime); return JS_STEP_ABRUPT; }
            /* §5.1's type, recorded the way the constructor records it: only where the init's own headers do
               not already name one, which stage 3's fill decides — so it is appended after that fill. */
            JS_FreeValue(ctx, s->body_mime);
            s->body_mime = mime ? JS_NewString(ctx, mime) : JS_UNDEFINED;
            free(mime);
        }
        s->stage = 1;
    }
    if (s->stage == 1) {
        /* 5.4 Request: a USVString names the URL directly; a Request names it through its `url` attribute, and
           THAT is the read that has to be a request. The INPUT itself is kept too: a Request carries a captured
           blob URL entry, which its `url` string cannot express. */
        if (JS_IsObject(input)) {
            r = step_getprop_run(ctx, &s->hdr, input, g_atom_url, cb_result, &s->url, out_cb, out_argc);
            if (r > 0) return r;
            if (r < 0) return JS_STEP_ABRUPT;
        } else {
            JS_FreeValue(ctx, cb_result);
            s->url = JS_DupValue(ctx, input);
        }
        /* THE DELIVERED VALUE IS CONSUMED HERE. Either branch above has taken it — the request answers with it
           and the other frees it — so the local must be cleared before the next stage, which now hands it to
           another request. It was left stale when this was the last stage and nothing read it again; the moment
           a stage was added after it, that stale copy was freed a second time. */
        cb_result = JS_UNDEFINED;
        s->stage = 2;
    }
    /* 5.4 Request step 33: `init.headers`, whose value is a HeadersInit. The READ is the page's code like the
       other two, and the CONVERSION is the page's code again — which is why the fill is a sub-sequence and not
       a loop here. Without it the endpoint's transport requirement was simply invisible: every request that
       needs an `Authorization` or an `X-Api-Version` was reported as if it needed nothing. */
    if (s->stage == 2) {
        if (JS_IsObject(init)) {
            r = step_getprop_run(ctx, &s->hdr, init, g_atom_headers, cb_result, &s->hinit, out_cb, out_argc);
            if (r > 0) return r;
            if (r < 0) return JS_STEP_ABRUPT;
        } else {
            JS_FreeValue(ctx, cb_result);
            /* NO init AT ALL, so there is no HeadersInit — and "no HeadersInit" has to be spelled `undefined`,
               not left as the state's zero. A js_mallocz'd JSValue reads as the INTEGER 0, which is neither
               undefined nor an object, so the fill rejected `fetch(url)` — every request with no init — with
               "a Headers init is not an object". The other two answers are written by their reads on every
               path that reaches them; this is the one a skipped read leaves behind. */
            s->hinit = JS_UNDEFINED;
        }
        cb_result = JS_UNDEFINED;
        headers_fill_init(&s->fill);
        s->stage = 3;
    }
    if (s->stage == 3) {
        /* §5.1: a request's header list has guard "request" — the names the browser owns (Host, Cookie,
           Origin, the method-override family carrying CONNECT/TRACE/TRACK) are dropped rather than sent. */
        r = headers_fill_run(ctx, &s->hdr, &s->fill, s->hinit, &s->hdrs, HEADERS_GUARD_REQUEST,
                             cb_result, out_cb, out_argc);
        if (r > 0) return r;
        if (r < 0) return JS_STEP_ABRUPT;
        cb_result = JS_UNDEFINED;
        s->stage = 4;
    }

    DCHECK(s->stage == 4, "the fetch machine was re-entered at a stage it never parks in");
    if (JS_IsString(s->method)) {
        mc = JS_ToCString(ctx, s->method);
        if (mc) method = mc;
    }
    {
        /* The surface's own header shape, built from the list — two fields either way, so this is a projection
           and not a second representation. The solver must not learn what a HeaderList is. */
        EndpointHeader *eh = NULL;
        int i;
        if (s->hdrs.n) {
            eh = js_malloc(ctx, sizeof(*eh) * (size_t)s->hdrs.n);
            if (!eh) { if (mc) JS_FreeCString(ctx, mc); return JS_STEP_ABRUPT; }
            for (i = 0; i < s->hdrs.n; i++) {
                eh[i].name = s->hdrs.e[i].name;
                eh[i].value = s->hdrs.e[i].value;
            }
        }
        endpoint_record(ctx, method, s->url, eh, s->hdrs.n);
        js_free(ctx, eh);
    }
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
        /* The reply's delivery, declared once for the runtime — every parked fetch mints a CLOSURE over this
           one definition rather than a definition per request. */
        g_deliver_stepid = JS_RegisterStepDef(rt, &js_fetch_deliver_def);
        response_init(ctx);
        headers_init(ctx);
        request_init(ctx);
        g_atom_method = JS_NewAtom(ctx, "method");
        g_atom_body   = JS_NewAtom(ctx, "body");
        g_atom_url    = JS_NewAtom(ctx, "url");
        g_atom_headers = JS_NewAtom(ctx, "headers");
        g_fetch_stepid = JS_RegisterStepDef(rt, &js_fetch_def);
    }
    headers_install(ctx, global);   /* §5's interface object — a page builds an init with it before it fetches */
    response_install(ctx, global);  /* §6's — a page constructs one to seed a cache or a service-worker path */
    request_install(ctx, global);   /* §5.3's — `fetch(new Request(u, init))` is how half of real code calls it */
    JS_SetPropertyStr(ctx, (JSValue)global, "fetch",
                      JS_NewCFunction2(ctx, NULL, "fetch", 1, JS_CFUNC_step, g_fetch_stepid));
}
