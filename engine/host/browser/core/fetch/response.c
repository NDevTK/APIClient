/* THE RESPONSE INTERFACE — WHATWG Fetch §6, over the body the trusted host supplied.
 *
 * `fetch()` resolved with the raw body STRING, which meant the shape every real bundle is written in —
 * `fetch(u).then(r => r.json())` — died at `r.json is not a function`, taking with it every endpoint, example
 * value and sink behind that request. The engine was learning the URL and then losing everything the reply
 * unlocked. `.then(eval)` happened to work, which is the rarer spelling and made the gap look narrower than it
 * was.
 *
 * WHAT IS REAL HERE. The body is the host's bytes, so `text()` hands them back and `json()` runs the REAL
 * JSON.parse over them — a spec codec is modelled by running it, never re-implemented, and a malformed body
 * therefore rejects with V8's own SyntaxError the way it does in a browser. `ok`/`status` are 200 because the
 * trusted host FETCHED this body successfully; a request that failed is the host's to report, and inventing a
 * concolic status here would fork every `if (r.ok)` in the page against a world the host never saw.
 * `bodyUsed` is the real single-use latch: a second read throws, which is what a page's own retry logic tests. */
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "quickjs-step.h"
#include "solver/cow.h"
#include "core/fetch/response.h"

static JSClassID g_response_class;
/* One WASM instance is one document, so the class id and the body readers' step ids below are that runtime's;
   the DCHECK at install is what makes a second one impossible rather than merely unlikely. */
static JSRuntime *g_response_rt;

/* THE BODY IS BYTES AND CARRIES ITS LENGTH. A body stream is a byte sequence in the spec, and it was a C string
   here: a reply with an interior NUL truncated at it silently, which `arrayBuffer()` and `bytes()` — the two
   readers whose entire job is to hand those bytes back — would then have reported as the whole body. There is
   no length a strlen can recover once the buffer is copied, so the length rides the record from the call that
   builds it, and `text()`/`json()` read exactly as far. */
typedef struct { char *url; char *body; size_t body_len; int body_used; } ResponseData;

static void response_finalizer(JSRuntime *rt, JSValue val)
{
    ResponseData *d = JS_GetOpaque(val, g_response_class);
    (void)rt;
    if (d) { js_free_rt(rt, d->url); js_free_rt(rt, d->body); js_free_rt(rt, d); }
}

static ResponseData *response_of(JSValueConst v) { return JS_GetOpaque(v, g_response_class); }

/* 6.4 "consume body": the latch is per Response and the second read is a TypeError — a page's retry path tests
   exactly that, so answering the body twice would hide the branch it takes.
   THE LATCH IS PER FLOW, TOO. It lives in this component's class opaque, which no property hook and no engine
   hook can see, so setting it was a write that did not time-travel: a Response created before a fork and read in
   one arm came back CONSUMED in the sibling, whose own first read then threw `body stream already read`. Every
   other kind of shared state a flow writes rides its COW delta, and so does this one now — the capture goes
   immediately before the write, so the bytes the delta records are the ones this flow found. */
static JSValue response_take_body(JSContext *ctx, JSValueConst this_val, const char **pbody, size_t *plen)
{
    ResponseData *d = response_of(this_val);
    if (!d)
        return JS_ThrowTypeError(ctx, "not a Response");
    if (d->body_used)
        return JS_ThrowTypeError(ctx, "body stream already read");
    cow_capture_host_state(ctx, this_val, &d->body_used, sizeof d->body_used);
    d->body_used = 1;
    *pbody = d->body ? d->body : "";
    *plen  = d->body ? d->body_len : 0;
    return JS_UNDEFINED;
}

/* 6.4 "consume body" AS A STEP MACHINE.
 *
 * The bytes are already here, so the promise this returns is settled before the page ever sees it — but SETTLING
 * it is not a C-private act. 27.2.1.3.2 step 8 reads `Get(resolution, "then")` off the value, which for
 * `json()`'s result is an ordinary object whose prototype the page owns: `Object.prototype.then = { get(){…} }`
 * makes that read the page's code, and prototype pollution is a gadget class this engine exists to run rather
 * than assume away. Performed with a JS_Call from C it would run in an activation with no flow base — a loop in
 * that getter would drive to completion — which is what the assert here used to say instead of fixing, and
 * `json()` therefore aborted on EVERY object body: `fetch(u).then(r => r.json())`, the shape every bundle is
 * written in. The resolving function is a CALL REQUEST now, so the read happens on the tramp where it can
 * suspend and fork, and both readers share the one machine because they differ only in how the value is
 * computed. */
enum { BODY_TEXT = 0, BODY_JSON = 1, BODY_ARRAYBUFFER = 2, BODY_BYTES = 3 };

typedef struct JSBodyState {
    JSStepHdr hdr;      /* FIRST — the driver writes the def and the operand bounds through it */
    uint8_t   stage;
    uint8_t   cphase;   /* the settle call's own phase, so the stage can hold it across a suspension */
    JSValue   promise;  /* the capability's promise — this machine's result (owned) */
    JSValue   func;     /* its resolve or its reject, whichever this body read settles with (owned) */
    JSValue   value;    /* what it settles WITH: the text, the parsed body, or the error (owned) */
    JSValue   cb[3];    /* the call request buffer: [this, resolving function, value] */
} JSBodyState;

/* WHAT THIS MACHINE OWNS. The call buffer is in here for the reason dispatch's is: a `then` getter that forks
   the flow must not leave two arms sharing one invocation. */
static void js_body_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSBodyState *s = st;
    int k;
    v->val(ctx, &s->promise);
    v->val(ctx, &s->func);
    v->val(ctx, &s->value);
    for (k = 0; k < 3; k++)
        v->val(ctx, &s->cb[k]);
}

static JSValue js_body_fini(JSContext *ctx, void *st, bool take_result)
{
    JSBodyState *s = st;
    JSValue r = take_result ? s->promise : JS_UNDEFINED;
    int k;

    if (take_result) s->promise = JS_UNDEFINED;
    JS_FreeValue(ctx, s->promise);
    JS_FreeValue(ctx, s->func);
    JS_FreeValue(ctx, s->value);
    s->promise = s->func = s->value = JS_UNDEFINED;
    for (k = 0; k < 3; k++) {
        JS_FreeValue(ctx, s->cb[k]);
        s->cb[k] = JS_UNDEFINED;
    }
    return r;
}

/* Stage 0 turns the bytes into the value this reader answers with; stage 1 settles the promise with it, which is
   the request. Nothing in stage 0 runs the page's code — the body is the host's bytes and the latch is this
   component's — which is why the four kinds are one stage and not four machines. json() parses with the REAL
   parser, so a malformed body rejects with the SyntaxError the page would actually catch rather than a
   placeholder this engine invented. */
static int js_body_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSBodyState *s = st;
    JSValue settled, funcs[2];
    int reject = 0, r;

    if (s->stage == 0) {
        const char *body = NULL;
        size_t len = 0;
        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        settled = response_take_body(ctx, s->hdr.this_val, &body, &len);
        if (JS_IsException(settled)) {
            s->value = JS_GetException(ctx);
            reject = 1;
        } else if (s->hdr.arg == BODY_JSON) {
            s->value = JS_ParseJSON(ctx, body, len, "<response>");
            if (JS_IsException(s->value)) { s->value = JS_GetException(ctx); reject = 1; }
        } else if (s->hdr.arg == BODY_ARRAYBUFFER || s->hdr.arg == BODY_BYTES) {
            /* 6.4.1 arrayBuffer() / 6.4.2 bytes(): the byte sequence itself, the two readers the string body
               could not have answered honestly. A COPY, because what the page gets is ITS OWN to detach,
               transfer or write through — handing out this Response's storage would let one flow mutate a
               reply another is still reading, and a detach would leave that flow reading freed memory. */
            s->value = s->hdr.arg == BODY_BYTES
                     ? JS_NewUint8ArrayCopy(ctx, (const uint8_t *)body, len)
                     : JS_NewArrayBufferCopy(ctx, (const uint8_t *)body, len);
            if (JS_IsException(s->value)) return JS_STEP_ABRUPT;
        } else {
            /* 6.4.5 text(): UTF-8 decode, which is what JS_NewStringLen performs over the host's bytes. */
            DCHECK(s->hdr.arg == BODY_TEXT, "the body-read machine was declared with a kind it does not read");
            s->value = JS_NewStringLen(ctx, body, len);
            if (JS_IsException(s->value)) return JS_STEP_ABRUPT;
        }
        /* The NATIVE capability: %Promise% with no subclass in sight, so building it constructs nothing of the
           page's. Only the settle below is the page's, and that is the request. */
        s->promise = JS_NewPromiseCapability(ctx, funcs);
        if (JS_IsException(s->promise)) return JS_STEP_ABRUPT;
        s->func = funcs[reject];
        JS_FreeValue(ctx, funcs[reject ^ 1]);
        s->stage = 1;
    }

    DCHECK(s->stage == 1, "the body-read machine was re-entered at a stage it never parks in");
    r = step_call_run(ctx, &s->cphase, s->cb, s->func, JS_UNDEFINED, 1, (JSValueConst *)&s->value,
                      cb_result, &settled, out_cb, out_argc);
    if (r > 0) return r;          /* parked ON THE SETTLE; the `then` read runs with a flow base under it */
    JS_FreeValue(ctx, settled);   /* a resolving function's return value is undefined and unobservable */
    return JS_STEP_DONE;
}

/* ONE machine, one def per body KIND — `arg` is what the step reads to decide how the bytes become a value, and
   it is the whole of the difference between the four. They are DECLARED as a list because that is what the
   registration and the install both walk: a fifth reader (6.4.3 blob(), once there is a Blob) is one row here
   and nothing else, and a row that is added without a step arm reaches the step's own DCHECK. */
static const struct { const char *name; JSTrampStepDef def; } js_body_readers[] = {
    { "text",        { sizeof(JSBodyState), js_body_step, js_body_fini, BODY_TEXT,        .visit = js_body_visit } },
    { "json",        { sizeof(JSBodyState), js_body_step, js_body_fini, BODY_JSON,        .visit = js_body_visit } },
    { "arrayBuffer", { sizeof(JSBodyState), js_body_step, js_body_fini, BODY_ARRAYBUFFER, .visit = js_body_visit } },
    { "bytes",       { sizeof(JSBodyState), js_body_step, js_body_fini, BODY_BYTES,       .visit = js_body_visit } },
};
#define BODY_READER_COUNT ((int)(sizeof(js_body_readers) / sizeof(js_body_readers[0])))
/* The ids JS_RegisterStepDef handed this runtime, one per row above. */
static int g_body_stepid[BODY_READER_COUNT];

/* 6.4 clone(). A caching or interceptor layer is written as `const copy = res.clone()` before it reads the
   body, because reading is single-use — so without this the FIRST thing such a bundle does with a reply is
   throw, and every endpoint, example value and sink behind that request is lost. It is the same failure `json`
   was added for, one layer further out.
   The spec's two steps are both load-bearing here. "If this is unusable, throw a TypeError" is SYNCHRONOUS —
   not a rejected promise — because clone is not a body-consuming method; a page that guards with try/catch
   sees the throw where it wrote the catch. And the clone gets its OWN body-used latch: the point of cloning is
   two independent reads, so sharing the latch would make the copy unreadable the moment the original was read
   and defeat the only reason the call exists. It does NOT consume this response — `res.clone()` leaves `res`
   readable, which is what the caching layer then does with it. */
static JSValue js_response_clone(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    ResponseData *d = response_of(this_val);
    (void)argc; (void)argv;
    if (!d)
        return JS_ThrowTypeError(ctx, "not a Response");
    if (d->body_used)
        return JS_ThrowTypeError(ctx, "cannot clone a Response whose body has been read");
    return response_new(ctx, d->url, d->body, d->body_len);
}

static JSValue js_response_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    ResponseData *d = response_of(this_val);
    if (!d)
        return JS_ThrowTypeError(ctx, "not a Response");
    switch (magic) {
    case 0: return JS_NewBool(ctx, true);                                    /* ok */
    case 1: return JS_NewInt32(ctx, 200);                                    /* status */
    case 2: return JS_NewString(ctx, "OK");                                  /* statusText */
    case 3: return JS_NewString(ctx, d->url ? d->url : "");                  /* url */
    case 4: return JS_NewBool(ctx, false);                                   /* redirected */
    case 5: return JS_NewString(ctx, "basic");                               /* type */
    default:
        DCHECK(magic == 6, "a Response accessor was declared with a magic this component does not answer");
        return JS_NewBool(ctx, d->body_used != 0);                           /* bodyUsed */
    }
}

static const JSCFunctionListEntry js_response_proto[] = {
    JS_CFUNC_DEF("clone", 0, js_response_clone),
    JS_CGETSET_MAGIC_DEF("ok", js_response_get, NULL, 0),
    JS_CGETSET_MAGIC_DEF("status", js_response_get, NULL, 1),
    JS_CGETSET_MAGIC_DEF("statusText", js_response_get, NULL, 2),
    JS_CGETSET_MAGIC_DEF("url", js_response_get, NULL, 3),
    JS_CGETSET_MAGIC_DEF("redirected", js_response_get, NULL, 4),
    JS_CGETSET_MAGIC_DEF("type", js_response_get, NULL, 5),
    JS_CGETSET_MAGIC_DEF("bodyUsed", js_response_get, NULL, 6),
};

void response_init(JSContext *ctx)
{
    JSClassDef def = { "Response", .finalizer = response_finalizer };
    JSRuntime *rt = JS_GetRuntime(ctx);
    int i;

    DCHECK(g_response_rt == NULL || g_response_rt == rt,
           "Response was installed into a second runtime — its class id and step ids belong to the first, and "
           "one WASM instance is one document");
    if (g_response_rt == rt)
        return;   /* IDEMPOTENT: the class and the body readers' step defs are the runtime's, and it has them */
    g_response_rt = rt;
    JS_NewClassID(rt, &g_response_class);
    JS_NewClass(rt, g_response_class, &def);
    for (i = 0; i < BODY_READER_COUNT; i++) {
        g_body_stepid[i] = JS_RegisterStepDef(rt, &js_body_readers[i].def);
        CHECK(g_body_stepid[i] >= 0, "response: no step id for a body reader — the reply would be unreadable");
    }
}

JSValue response_new(JSContext *ctx, const char *url, const char *body, size_t body_len)
{
    ResponseData *d;
    JSValue obj;
    int i;

    DCHECK(g_response_class != 0, "a Response was built before the class existed — response_init runs at install");
    obj = JS_NewObjectClass(ctx, g_response_class);
    if (JS_IsException(obj))
        return obj;
    d = js_mallocz(ctx, sizeof(*d));
    if (!d) { JS_FreeValue(ctx, obj); return JS_EXCEPTION; }
    d->url = js_strdup(ctx, url ? url : "");
    /* +1 and a NUL past the end, so the bytes can still be handed to a C string consumer; `body_len` is what
       every read here uses, and it is what an interior NUL no longer truncates. */
    d->body = js_mallocz(ctx, body_len + 1);
    CHECK(d->url && d->body, "response: OOM copying a reply — a dropped body loses everything behind the request");
    if (body_len) memcpy(d->body, body, body_len);
    d->body_len = body_len;
    JS_SetOpaque(obj, d);
    JS_SetPropertyFunctionList(ctx, obj, js_response_proto,
                               (int)(sizeof(js_response_proto) / sizeof(js_response_proto[0])));
    for (i = 0; i < BODY_READER_COUNT; i++) {
        DCHECK(g_body_stepid[i] > 0,
               "a Response was built before its body readers were registered — response_init runs at install");
        JS_SetPropertyStr(ctx, obj, js_body_readers[i].name,
                          JS_NewCFunction2(ctx, NULL, js_body_readers[i].name, 0, JS_CFUNC_step,
                                           g_body_stepid[i]));
    }
    return obj;
}
