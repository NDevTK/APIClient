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
#include "core/fetch/headers.h"
#include "core/idl_args.h"

static JSClassID g_response_class;
static JSValue   g_response_proto = JS_UNDEFINED;
static int       g_ctor_stepid = -1;
/* One WASM instance is one document, so the class id and the body readers' step ids below are that runtime's;
   the DCHECK at install is what makes a second one impossible rather than merely unlikely. */
static JSRuntime *g_response_rt;

/* THE BODY IS BYTES AND CARRIES ITS LENGTH. A body stream is a byte sequence in the spec, and it was a C string
   here: a reply with an interior NUL truncated at it silently, which `arrayBuffer()` and `bytes()` — the two
   readers whose entire job is to hand those bytes back — would then have reported as the whole body. There is
   no length a strlen can recover once the buffer is copied, so the length rides the record from the call that
   builds it, and `text()`/`json()` read exactly as far. */
/* §6.4's RESPONSE, as the spec's fields and not as constants. `ok`/`status`/`statusText`/`type` were literals
   here — 200, "OK", "basic" — which was right for the one Response this component could build (the reply the
   trusted host had already fetched) and wrong the moment the page can build one: `new Response("", {status:
   404})` is not 200, and every `if (r.ok)` in a page that constructs its own responses forked the wrong way.
   `has_body` is not `body_len != 0`: §6.4 distinguishes a NULL body from an empty one, `new Response()` has
   the first and `new Response("")` the second, and `.body` reports null for exactly one of them. */
typedef struct {
    char   *url;              /* the response's URL; "" when it has none, which is what `url` reports */
    char   *body;
    size_t  body_len;
    int     body_used;
    int     has_body;         /* §6.4: a null body is not an empty body */
    int     status;
    char   *status_text;      /* the response's "status message" */
    uint8_t type;             /* RESPONSE_TYPE_* */
    /* [SameObject]: §6.4 declares `headers` SameObject, so the Headers OBJECT is built once and held here
       rather than minted per read — a page that does `const h = r.headers; h.set(...)` must be writing into
       the response's own list, and `r.headers === r.headers` is asserted by wpt directly. */
    JSValue headers;
} ResponseData;

/* §6.4's response types. "default" is what the CONSTRUCTOR makes; "basic" is a same-origin reply the host
   fetched. The others arrive with CORS and opaque replies, which is where they will be set from. */
enum { RESPONSE_TYPE_DEFAULT = 0, RESPONSE_TYPE_BASIC, RESPONSE_TYPE_CORS, RESPONSE_TYPE_ERROR,
       RESPONSE_TYPE_OPAQUE, RESPONSE_TYPE_OPAQUEREDIRECT };
static const char *const RESPONSE_TYPE_NAME[] = { "default", "basic", "cors", "error", "opaque",
                                                  "opaqueredirect" };

static void response_finalizer(JSRuntime *rt, JSValue val)
{
    ResponseData *d = JS_GetOpaque(val, g_response_class);
    if (d) {
        JS_FreeValueRT(rt, d->headers);
        js_free_rt(rt, d->url); js_free_rt(rt, d->status_text); js_free_rt(rt, d->body); js_free_rt(rt, d);
    }
}

/* The Headers object a Response holds is a JSValue in the class opaque, which the collector cannot see through
   — and a Headers can reach back (nothing today, but the cycle is the collector's problem to be told about,
   not the component's to promise will not form). */
static void response_gc_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    ResponseData *d = JS_GetOpaque(val, g_response_class);
    if (d) JS_MarkValue(rt, d->headers, mark_func);
}

static ResponseData *response_of(JSValueConst v) { return JS_GetOpaque(v, g_response_class); }

/* Allocation and filling, declared here because clone() is written above them and because splitting them is
   what lets the constructor allocate before it knows the body — see their definitions. */
static JSValue response_alloc(JSContext *ctx, ResponseData **pd);
static int response_set(JSContext *ctx, ResponseData *d, const char *url, int status, const char *status_text,
                        int type, const char *body, size_t body_len, const HeaderList *headers,
                        HeadersGuard guard);

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
    ResponseData *d = response_of(this_val), *c;
    JSValue obj;
    (void)argc; (void)argv;
    if (!d)
        return JS_ThrowTypeError(ctx, "not a Response");
    if (d->body_used)
        return JS_ThrowTypeError(ctx, "cannot clone a Response whose body has been read");
    obj = response_alloc(ctx, &c);
    if (JS_IsException(obj))
        return obj;
    /* §6.4 clone COPIES the response, so every field goes — a clone that kept only the body reported status
       200 and type "basic" for a 404 the page had just constructed. The headers are a NEW Headers over the
       same list, because [SameObject] is per response and `r.clone().headers === r.headers` is false. */
    if (response_set(ctx, c, d->url, d->status, d->status_text, d->type,
                     d->has_body ? d->body : NULL, d->body_len, headers_list_of(d->headers),
                     headers_guard_of(d->headers)) < 0) {
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    return obj;
}

static JSValue js_response_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    ResponseData *d = response_of(this_val);
    if (!d)
        return JS_ThrowTypeError(ctx, "not a Response");
    switch (magic) {
    /* §6.4 `ok` IS the 200..299 test, not a flag: a page's `if (r.ok)` and its `if (r.status < 400)` must
       agree, and they only do when both read the same number. */
    case 0: return JS_NewBool(ctx, d->status >= 200 && d->status <= 299);
    case 1: return JS_NewInt32(ctx, d->status);
    case 2: return JS_NewString(ctx, d->status_text ? d->status_text : "");
    case 3: return JS_NewString(ctx, d->url ? d->url : "");
    case 4: return JS_NewBool(ctx, false);                                   /* redirected: no redirect yet */
    case 5: return JS_NewString(ctx, RESPONSE_TYPE_NAME[d->type]);
    case 6: return JS_DupValue(ctx, d->headers);                             /* [SameObject] */
    default:
        DCHECK(magic == 7, "a Response accessor was declared with a magic this component does not answer");
        return JS_NewBool(ctx, d->body_used != 0);                           /* bodyUsed */
    }
}

/* §6.4's attributes and clone(). `body` IS NOT HERE, and its absence is the honest report: the spec's type is
   `ReadableStream?`, this engine has no ReadableStream, and a getter answering null for a response that HAS a
   body would be a lie the page cannot tell from the null-body case. `blob()` and `formData()` are absent for
   the same reason — there is no Blob and no FormData to answer with. */
static const JSCFunctionListEntry js_response_proto_funcs[] = {
    JS_CFUNC_DEF("clone", 0, js_response_clone),
    JS_CGETSET_MAGIC_DEF("ok", js_response_get, NULL, 0),
    JS_CGETSET_MAGIC_DEF("status", js_response_get, NULL, 1),
    JS_CGETSET_MAGIC_DEF("statusText", js_response_get, NULL, 2),
    JS_CGETSET_MAGIC_DEF("url", js_response_get, NULL, 3),
    JS_CGETSET_MAGIC_DEF("redirected", js_response_get, NULL, 4),
    JS_CGETSET_MAGIC_DEF("type", js_response_get, NULL, 5),
    JS_CGETSET_MAGIC_DEF("headers", js_response_get, NULL, 6),
    JS_CGETSET_MAGIC_DEF("bodyUsed", js_response_get, NULL, 7),
};

/* ---- the interface: one prototype, one instance shape ---------------------------------------------------- */

/* An EMPTY Response over the interface prototype. Every field is set by response_set afterwards; splitting the
   two is what lets the constructor allocate before it knows the status and lets clone copy field by field. */
static JSValue response_alloc(JSContext *ctx, ResponseData **pd)
{
    ResponseData *d;
    JSValue obj;

    DCHECK(g_response_class != 0, "a Response was built before the class existed — response_init runs at install");
    DCHECK(!JS_IsUndefined(g_response_proto), "a Response was built before its prototype existed");
    obj = JS_NewObjectProtoClass(ctx, g_response_proto, g_response_class);
    if (JS_IsException(obj))
        return obj;
    d = js_mallocz(ctx, sizeof(*d));
    if (!d) { JS_FreeValue(ctx, obj); return JS_EXCEPTION; }
    d->headers = JS_UNDEFINED;
    d->status = 200;
    JS_SetOpaque(obj, d);
    *pd = d;
    return obj;
}

/* Fill a freshly allocated response. `body` NULL is the spec's NULL BODY, which is not the empty body. Returns
   -1 with an exception live. */
static int response_set(JSContext *ctx, ResponseData *d, const char *url, int status, const char *status_text,
                        int type, const char *body, size_t body_len, const HeaderList *headers,
                        HeadersGuard guard)
{
    d->url = js_strdup(ctx, url ? url : "");
    d->status_text = js_strdup(ctx, status_text ? status_text : "");
    if (!d->url || !d->status_text) return -1;
    d->status = status;
    d->type = (uint8_t)type;
    d->has_body = body != NULL;
    if (body) {
        /* +1 and a NUL past the end, so the bytes can still be handed to a C string consumer; `body_len` is
           what every read here uses, and it is what an interior NUL no longer truncates. */
        d->body = js_mallocz(ctx, body_len + 1);
        if (!d->body) return -1;
        if (body_len) memcpy(d->body, body, body_len);
        d->body_len = body_len;
    }
    JS_FreeValue(ctx, d->headers);
    d->headers = headers_new(ctx, headers, guard);
    return JS_IsException(d->headers) ? -1 : 0;
}

/* ---- §6.4's constructor ----------------------------------------------------------------------------------
 *
 * A MACHINE, because both of its arguments run the page's code: the `body` is a `BodyInit?` union whose
 * USVString arm is a ToString the page may define, and `init` is a dictionary whose three members are three
 * [[Get]]s — one Proxy trap away each — followed by the header fill, which is the whole iterator or record
 * protocol over an object the page owns. The declaration converts both; what is left here is §6.4's own steps,
 * in the order the spec writes them. */
typedef struct {
    uint8_t     stage;
    HeadersFill fill;
    HeaderList  list;
    JSValue     result;
} JSResponseCtorState;

static void js_response_ctor_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSResponseCtorState *s = st;
    headers_fill_visit(ctx, &s->fill, v);
    v->val(ctx, &s->result);
}

static void js_response_ctor_release(JSContext *ctx, void *st)
{
    JSResponseCtorState *s = st;
    headers_fill_release(ctx, &s->fill);
    header_list_free(&s->list);
    JS_FreeValue(ctx, s->result);
    s->result = JS_UNDEFINED;
}

/* §6.4 "a null body status": the statuses HTTP defines as carrying no body, which a constructed Response may
   therefore not be given one with. */
static int response_is_null_body_status(int status)
{
    return status == 101 || status == 103 || status == 204 || status == 205 || status == 304;
}

/* §5.1's `reason-phrase` production: HTAB, SP, VCHAR and obs-text — and nothing else, which is what makes
   `new Response("", {statusText: "\n"})` a TypeError. */
static int response_status_text_is_valid(const char *s, size_t len)
{
    size_t i;
    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == 0x09 || (c >= 0x20 && c <= 0x7e) || c >= 0x80) continue;
        return 0;
    }
    return 1;
}

static int js_response_ctor_step(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                                 JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    JSResponseCtorState *s = st;
    JSValueConst body = argc > 0 ? argv[0] : JS_UNDEFINED;
    JSValueConst init = argc > 1 ? argv[1] : JS_UNDEFINED;
    JSValue v_status, v_text, v_headers;
    ResponseData *d;
    int status, r;

    if (s->stage == 0) {
        if (JS_IsUndefined(hdr->this_val)) {
            JS_FreeValue(ctx, cb_result);
            JS_ThrowTypeError(ctx, "constructor Response requires 'new'");
            return -1;
        }
        /* §6.4 step 1: the RANGE check, after Web IDL's `unsigned short` conversion and never instead of it. */
        v_status = idl_dict_get(ctx, init, "status");
        status = 200;
        if (!JS_IsUndefined(v_status)) {
            int32_t n = 200;
            JS_ToInt32(ctx, &n, v_status);   /* already an integer the declaration converted; this reads it */
            status = (int)n;
        }
        JS_FreeValue(ctx, v_status);
        if (status < 200 || status > 599) {
            JS_FreeValue(ctx, cb_result);
            JS_ThrowRangeError(ctx, "a Response status must be in the range 200 to 599");
            return -1;
        }
        /* §6.4 step 2: statusText must match reason-phrase. */
        v_text = idl_dict_get(ctx, init, "statusText");
        {
            const char *t = NULL;
            size_t tlen = 0;
            int ok = 1;
            if (!JS_IsUndefined(v_text)) {
                t = JS_ToCStringLen(ctx, &tlen, v_text);
                if (!t) { JS_FreeValue(ctx, v_text); JS_FreeValue(ctx, cb_result); return -1; }
                ok = response_status_text_is_valid(t, tlen);
            }
            if (!ok) {
                JS_FreeCString(ctx, t);
                JS_FreeValue(ctx, v_text);
                JS_FreeValue(ctx, cb_result);
                JS_ThrowTypeError(ctx, "a Response statusText is not a reason-phrase");
                return -1;
            }
            /* §6.4 steps 3-6: the response exists now, with its status and its status message. The BODY and
               the HEADERS are the two things still to come, and both may run the page's code. */
            s->result = response_alloc(ctx, &d);
            if (JS_IsException(s->result)) {
                JS_FreeCString(ctx, t);
                JS_FreeValue(ctx, v_text);
                JS_FreeValue(ctx, cb_result);
                return -1;
            }
            r = response_set(ctx, d, "", status, t ? t : "", RESPONSE_TYPE_DEFAULT, NULL, 0, NULL,
                             HEADERS_GUARD_RESPONSE);
            JS_FreeCString(ctx, t);
            JS_FreeValue(ctx, v_text);
            if (r < 0) { JS_FreeValue(ctx, cb_result); return -1; }
        }
        headers_fill_init(&s->fill);
        s->stage = 1;
    }

    if (s->stage == 1) {
        /* §6.4 step 7: fill the response's headers, which have guard "response" — so a page that puts
           Set-Cookie on a Response it built silently gets nothing, which is the header wpt asserts by name. */
        v_headers = idl_dict_get(ctx, init, "headers");
        r = headers_fill_run(ctx, hdr, &s->fill, v_headers, &s->list, HEADERS_GUARD_RESPONSE,
                             cb_result, out_cb, out_argc);
        JS_FreeValue(ctx, v_headers);
        if (r > 0) return r;
        if (r < 0) return -1;
        cb_result = JS_UNDEFINED;
        d = response_of(s->result);
        DCHECK(d != NULL, "the Response the constructor allocated stopped being one mid-construction");
        JS_FreeValue(ctx, d->headers);
        d->headers = headers_new(ctx, &s->list, HEADERS_GUARD_RESPONSE);
        if (JS_IsException(d->headers)) return -1;
        s->stage = 2;
    }

    DCHECK(s->stage == 2, "the Response constructor was re-entered at a stage it never parks in");
    JS_FreeValue(ctx, cb_result);
    d = response_of(s->result);
    /* §6.4 step 8: a non-null body. The declaration has already run the union — a BufferSource crossed as
       itself and everything else is a USVString — so what is left is the two things the SPEC does here: refuse
       a body on a null-body status, and set Content-Type when the extracted body has one and the header list
       does not already. */
    if (!JS_IsNull(body) && !JS_IsUndefined(body)) {
        const uint8_t *bytes = NULL;
        size_t len = 0;
        JSValue buf = JS_UNDEFINED;
        const char *str = NULL;
        const char *mime = NULL;

        if (response_is_null_body_status(d->status)) {
            JS_ThrowTypeError(ctx, "a Response with a null body status cannot be given a body");
            return -1;
        }
        if (JS_IsArrayBuffer(body)) {
            bytes = JS_GetArrayBuffer(ctx, &len, body);
            if (!bytes) return -1;
        } else if (JS_IsObject(body)) {
            size_t off = 0;
            buf = JS_GetArrayBufferView(ctx, body, &off, &len);
            DCHECK(!JS_IsException(buf),
                   "the BodyInit union let through an object that is neither a BufferSource nor a string — the "
                   "declaration converts the USVString arm, so nothing else can reach here");
            {
                size_t whole = 0;
                uint8_t *base = JS_GetArrayBuffer(ctx, &whole, buf);
                if (!base) { JS_FreeValue(ctx, buf); return -1; }
                bytes = base + off;
            }
        } else {
            /* the USVString arm: the declaration already ran ToString, so this is the string's bytes */
            str = JS_ToCStringLen(ctx, &len, body);
            if (!str) return -1;
            bytes = (const uint8_t *)str;
            /* §6.4: a USVString body's Content-Type. A BufferSource has none, which is why this is set here
               and not for every body. */
            mime = "text/plain;charset=UTF-8";
        }
        d->body = js_mallocz(ctx, len + 1);
        if (!d->body) { JS_FreeCString(ctx, str); JS_FreeValue(ctx, buf); return -1; }
        if (len) memcpy(d->body, bytes, len);
        d->body_len = len;
        d->has_body = 1;
        JS_FreeCString(ctx, str);
        JS_FreeValue(ctx, buf);
        if (mime) {
            const HeaderList *hl = headers_list_of(d->headers);
            char *existing = header_list_get(hl, "content-type");
            if (!existing)
                header_list_append((HeaderList *)hl, "content-type", mime);
            free(existing);
        }
    }
    *presult = s->result;
    s->result = JS_UNDEFINED;
    return 0;
}

/* §6.4 error(): "a new response whose type is 'error'". It runs none of the page's code — no arguments, no
   init — so it is an ordinary member and not a machine. Its status is 0, which is outside the range the
   CONSTRUCTOR enforces, because that range is the constructor's step and not the response's. */
static JSValue js_response_error(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    ResponseData *d;
    JSValue obj = response_alloc(ctx, &d);
    (void)this_val; (void)argc; (void)argv;
    if (JS_IsException(obj))
        return obj;
    if (response_set(ctx, d, "", 0, "", RESPONSE_TYPE_ERROR, NULL, 0, NULL, HEADERS_GUARD_IMMUTABLE) < 0) {
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    return obj;
}

static const JSCFunctionListEntry js_response_static_funcs[] = {
    JS_CFUNC_DEF("error", 0, js_response_error),
};

static const IdlStepDecl js_response_ctor_decl = {
    js_response_ctor_step, sizeof(JSResponseCtorState), js_response_ctor_visit, js_response_ctor_release
};

/* ---- install --------------------------------------------------------------------------------------------- */

void response_init(JSContext *ctx)
{
    JSClassDef def = { "Response", .finalizer = response_finalizer, .gc_mark = response_gc_mark };
    JSRuntime *rt = JS_GetRuntime(ctx);
    /* `optional BodyInit? body = null, optional ResponseInit init = {}`. The dictionary's members are listed in
       the order Web IDL reads them, which is lexicographic and not the order §6.4 uses them in. */
    static const IdlArgType CTOR_ARGS[2] = { IDL_BODYINIT_NULLABLE, IDL_DICT };
    static const IdlDictMember RESPONSE_INIT[3] = {
        { "headers",    IDL_ANY,            false },   /* HeadersInit: the union the fill converts */
        { "status",     IDL_UNSIGNED_SHORT, false },
        { "statusText", IDL_BYTESTRING,     false },   /* the reason-phrase check on top is §6.4's */
    };
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
    /* ONE PROTOTYPE, not a copy of the members per instance. They were own properties of each Response, which
       is not what Web IDL describes and is observable three ways: `Response.prototype.text` was absent,
       `delete r.text` removed the method, and every reply paid for eight property installs. */
    g_response_proto = JS_NewObject(ctx);
    CHECK(!JS_IsException(g_response_proto), "Response.prototype could not be allocated");
    JS_SetPropertyFunctionList(ctx, g_response_proto, js_response_proto_funcs,
                               (int)(sizeof(js_response_proto_funcs) / sizeof(js_response_proto_funcs[0])));
    for (i = 0; i < BODY_READER_COUNT; i++)
        JS_SetPropertyStr(ctx, g_response_proto, js_body_readers[i].name,
                          JS_NewCFunction2(ctx, NULL, js_body_readers[i].name, 0, JS_CFUNC_step,
                                           g_body_stepid[i]));
    g_ctor_stepid = idl_method_id_step(ctx, CTOR_ARGS, 2, RESPONSE_INIT, 3, &js_response_ctor_decl, 0);
}

void response_install(JSContext *ctx, JSValueConst global)
{
    JSValue ctor;

    DCHECK(g_ctor_stepid >= 0, "Response was installed before response_init declared its constructor");
    ctor = idl_step_constructor(ctx, "Response", 0, g_ctor_stepid);
    CHECK(!JS_IsException(ctor), "the Response interface object could not be allocated");
    JS_SetConstructor(ctx, ctor, g_response_proto);
    JS_SetPropertyFunctionList(ctx, ctor, js_response_static_funcs,
                               (int)(sizeof(js_response_static_funcs) / sizeof(js_response_static_funcs[0])));
    JS_SetPropertyStr(ctx, (JSValue)global, "Response", ctor);
}

void response_free(JSContext *ctx)
{
    JS_FreeValue(ctx, g_response_proto);
    g_response_proto = JS_UNDEFINED;
    g_response_rt = NULL;
    g_ctor_stepid = -1;
}

/* THE REPLY THE TRUSTED HOST FETCHED — type "basic", status 200, and headers the page may not write, which is
   the §6.4 "immutable" guard and the reason that guard exists at all. */
JSValue response_new(JSContext *ctx, const char *url, const char *body, size_t body_len)
{
    ResponseData *d;
    JSValue obj = response_alloc(ctx, &d);

    if (JS_IsException(obj))
        return obj;
    if (response_set(ctx, d, url, 200, "OK", RESPONSE_TYPE_BASIC, body ? body : "", body_len, NULL,
                     HEADERS_GUARD_IMMUTABLE) < 0) {
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    return obj;
}
