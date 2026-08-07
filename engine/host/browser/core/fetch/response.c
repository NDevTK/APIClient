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
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "quickjs-step.h"
#include "solver/cow.h"
#include "core/fetch/fetch.h"
#include "core/fetch/response.h"
#include "core/fetch/headers.h"
#include "core/idl_args.h"
#include "core/fetch/body.h"
#include "core/streams/readable_stream.h"

static JSClassID g_response_class;
static JSValue   g_response_proto = JS_UNDEFINED;
static int       g_ctor_stepid = -1, g_json_stepid = -1, g_redirect_stepid = -1;
static int       g_body_handle = -1;
/* One WASM instance is one document, so the class id and the body readers' step ids below are that runtime's;
   the DCHECK at install is what makes a second one impossible rather than merely unlikely. */
static JSRuntime *g_response_rt;
/* %JSON.stringify%, captured at install. §6.4's "serialize a JavaScript value to JSON bytes" is the ABSTRACT
   operation, not a property read — a page that replaces `JSON.stringify` does not thereby change what
   `Response.json` does — so the original function object is held rather than looked up per call. It is read
   before any page script runs, off the engine's own builtin, so the capture itself runs nothing of the
   page's. */
static JSValue g_json_stringify = JS_UNDEFINED;

static JSValueConst response_json_stringify(void)
{
    DCHECK(!JS_IsUndefined(g_json_stringify),
           "Response.json ran before response_init captured %JSON.stringify%");
    return g_json_stringify;
}

/* THE BODY IS BYTES AND CARRIES ITS LENGTH. A body stream is a byte sequence in the spec, and it was a C string
   here: a reply with an interior NUL truncated at it silently, which `arrayBuffer()` and `bytes()` — the two
   readers whose entire job is to hand those bytes back — would then have reported as the whole body. There is
   no length a strlen can recover once the buffer is copied, so the length rides the record from the call that
   builds it, and `text()`/`json()` read exactly as far. */
/* §6.4's RESPONSE, as the spec's fields and not as constants. `ok`/`status`/`statusText`/`type` were literals
   here — 200, "OK", "basic" — which was right for the one Response this component could build (the reply the
   trusted host had already fetched) and wrong the moment the page can build one: `new Response("", {status:
   404})` is not 200, and every `if (r.ok)` in a page that constructs its own responses forked the wrong way.
   The BODY is §5.2's mixin, whose one implementation Request includes too. */
typedef struct {
    char   *url;              /* the response's URL; "" when it has none, which is what `url` reports */
    BodyState body;           /* §5.2's mixin, one implementation shared with Request */
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
        body_state_free(rt, &d->body);
        js_free_rt(rt, d->url); js_free_rt(rt, d->status_text); js_free_rt(rt, d);
    }
}

/* The Headers object a Response holds is a JSValue in the class opaque, which the collector cannot see through
   — and a Headers can reach back (nothing today, but the cycle is the collector's problem to be told about,
   not the component's to promise will not form). */
static void response_gc_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    ResponseData *d = JS_GetOpaque(val, g_response_class);
    if (d) JS_MarkValue(rt, d->headers, mark_func);
    if (d) body_state_mark(rt, &d->body, mark_func);
}

static ResponseData *response_of(JSValueConst v) { return JS_GetOpaque(v, g_response_class); }

/* How §5.2's shared readers find this interface's body. */
/* §5.2's `formData()` asks the including interface for its Content-Type, because only it knows where its
   headers live. NULL when there is none, which is a body with no form encoding and therefore a TypeError. */
static char *response_body_mime(JSContext *ctx, JSValueConst v)
{
    ResponseData *d = JS_GetOpaque(v, g_response_class);
    (void)ctx;
    return d ? header_list_get(headers_list_of(d->headers), "content-type") : NULL;
}

static BodyState *response_body_of(JSValueConst v)
{
    ResponseData *d = JS_GetOpaque(v, g_response_class);
    return d ? &d->body : NULL;
}

/* Allocation and filling, declared here because clone() is written above them and because splitting them is
   what lets the constructor allocate before it knows the body — see their definitions. */
static JSValue response_alloc(JSContext *ctx, ResponseData **pd);
static int response_set(JSContext *ctx, ResponseData *d, const char *url, int status, const char *status_text,
                        int type, const char *body, size_t body_len, const HeaderList *headers,
                        HeadersGuard guard);

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
/* IT IS A STEP, because §5.2's "clone a body" is a TEE and a tee is a call of code the page can reach. The two
   synchronous refusals still happen where the page wrote its try/catch: a locked or disturbed body is a
   TypeError from clone() itself, not a rejected promise, because clone is not a body-consuming method. */
typedef struct {
    uint8_t stage, phase;
    JSValue obj;       /* the clone being built (owned) */
    JSValue cb[2];     /* step_call_run's buffer: [this, tee] — the tee takes no arguments */
} JSCloneState;

enum { CL_ENTRY = 0, CL_BODY, CL_DONE };

static void js_clone_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSCloneState *s = st;
    int k;
    v->val(ctx, &s->obj);
    for (k = 0; k < 2; k++) v->val(ctx, &s->cb[k]);
}

static void js_clone_release(JSContext *ctx, void *st)
{
    JSCloneState *s = st;
    int k;
    JS_FreeValue(ctx, s->obj);
    s->obj = JS_UNDEFINED;
    for (k = 0; k < 2; k++) { JS_FreeValue(ctx, s->cb[k]); s->cb[k] = JS_UNDEFINED; }
}

static int js_response_clone_step(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                                  JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    JSCloneState *s = st;
    ResponseData *d = response_of(hdr->this_val), *c;
    int r;

    (void)argc; (void)argv;
    if (s->stage == CL_ENTRY) {
        bool locked = false;
        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        s->obj = JS_UNDEFINED;
        s->cb[0] = s->cb[1] = JS_UNDEFINED;
        if (!d)
            return JS_ThrowTypeError(ctx, "not a Response"), -1;
        /* §5.2's "unusable" is DISTURBED **or** LOCKED — a body whose stream a page has taken a reader on
           cannot be teed, and reporting only the read latch let clone() reach the tee and throw from inside
           it, which is the wrong error at the wrong place. */
        readable_stream_query(d->body.stream, NULL, &locked);
        if (d->body.used || readable_stream_disturbed(d->body.stream) || locked)
            return JS_ThrowTypeError(ctx, "cannot clone a Response whose body is disturbed or locked"), -1;
        s->obj = response_alloc(ctx, &c);
        if (JS_IsException(s->obj)) { s->obj = JS_UNDEFINED; return -1; }
        /* §6.4 clone COPIES the response, so every field goes — a clone that kept only the body reported status
           200 and type "basic" for a 404 the page had just constructed. The headers are a NEW Headers over the
           same list, because [SameObject] is per response and `r.clone().headers === r.headers` is false. The
           BODY is not among them: it is the tee below, not a copy. */
        if (response_set(ctx, c, d->url, d->status, d->status_text, d->type, NULL, 0,
                         headers_list_of(d->headers), headers_guard_of(d->headers)) < 0)
            return -1;
        s->stage = CL_BODY;
    }

    DCHECK(s->stage == CL_BODY, "Response.clone resumed in a stage it never parks in");
    DCHECK(d != NULL, "a Response stopped being one between two stages of its own clone");
    c = response_of(s->obj);
    DCHECK(c != NULL, "the clone this machine allocated stopped being a Response");
    r = body_clone_run(ctx, &s->phase, s->cb, 2, &c->body, &d->body, cb_result, out_cb, out_argc);
    if (r > 0) return r;
    if (r < 0) return -1;
    s->stage = CL_DONE;
    *presult = s->obj;
    s->obj = JS_UNDEFINED;
    return 0;
}

static const IdlStepDecl js_response_clone_decl = {
    js_response_clone_step, sizeof(JSCloneState), js_clone_visit, js_clone_release
};

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
    default:
        DCHECK(magic == 6, "a Response accessor was declared with a magic this component does not answer");
        return JS_DupValue(ctx, d->headers);                                 /* [SameObject] */
    }
}

/* §6.4's attributes and clone(). `body` IS NOT HERE, and its absence is the honest report: the spec's type is
   `ReadableStream?`, this engine has no ReadableStream, and a getter answering null for a response that HAS a
   body would be a lie the page cannot tell from the null-body case. `blob()` and `formData()` are absent for
   the same reason — there is no Blob and no FormData to answer with. */
static const JSCFunctionListEntry js_response_proto_funcs[] = {
    JS_CGETSET_MAGIC_DEF("ok", js_response_get, NULL, 0),
    JS_CGETSET_MAGIC_DEF("status", js_response_get, NULL, 1),
    JS_CGETSET_MAGIC_DEF("statusText", js_response_get, NULL, 2),
    JS_CGETSET_MAGIC_DEF("url", js_response_get, NULL, 3),
    JS_CGETSET_MAGIC_DEF("redirected", js_response_get, NULL, 4),
    JS_CGETSET_MAGIC_DEF("type", js_response_get, NULL, 5),
    JS_CGETSET_MAGIC_DEF("headers", js_response_get, NULL, 6),
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
    if (body_state_set(ctx, &d->body, body, body_len) < 0) return -1;
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
/* ONE MACHINE FOR THE CONSTRUCTOR AND FOR json(), because §6.4 gives them ONE algorithm: both create a
   response and then run "initialize a response" over the same init dictionary. They differ in exactly two
   places — where the BODY comes from, and whether `new` is required — and a second machine would have been a
   second copy of the status range check, the reason-phrase check, the header fill and the Content-Type rule.
   The magic says which entry this is. */
enum { RESP_ENTRY_CTOR = 0, RESP_ENTRY_JSON };
/* The stages, with a START of its own: a zeroed step state's JSValue is the INTEGER 0 and not JS_UNDEFINED, so
   "have I begun" is only ever readable off the stage byte. */
enum { RESP_START = 0, RESP_SERIALIZE, RESP_INIT, RESP_HEADERS, RESP_BODY };

typedef struct {
    uint8_t     stage;
    uint8_t     cphase;    /* json(): the JSON.stringify call's own phase, held across the stage */
    HeadersFill fill;
    HeaderList  list;
    JSValue     result;
    JSValue     json;      /* json(): what the serialization produced (owned) */
    JSValue     cb[3];     /* json(): the call request buffer — [this, JSON.stringify, data] */
} JSResponseCtorState;

static void js_response_ctor_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSResponseCtorState *s = st;
    int k;
    headers_fill_visit(ctx, &s->fill, v);
    v->val(ctx, &s->result);
    v->val(ctx, &s->json);
    for (k = 0; k < 3; k++) v->val(ctx, &s->cb[k]);
}

static void js_response_ctor_release(JSContext *ctx, void *st)
{
    JSResponseCtorState *s = st;
    int k;
    headers_fill_release(ctx, &s->fill);
    header_list_free(&s->list);
    JS_FreeValue(ctx, s->result);
    JS_FreeValue(ctx, s->json);
    s->result = s->json = JS_UNDEFINED;
    for (k = 0; k < 3; k++) { JS_FreeValue(ctx, s->cb[k]); s->cb[k] = JS_UNDEFINED; }
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
    int entry = idl_step_magic(hdr);
    JSValueConst body = argc > 0 ? argv[0] : JS_UNDEFINED;
    JSValueConst init = argc > 1 ? argv[1] : JS_UNDEFINED;
    JSValue v_status, v_text, v_headers;
    ResponseData *d;
    int status, r;

    if (s->stage == RESP_START) {
        /* `new` is the CONSTRUCTOR's requirement. json() is a static, so its receiver is the interface object
           and there is nothing to require. */
        if (entry == RESP_ENTRY_CTOR && JS_IsUndefined(hdr->this_val)) {
            JS_FreeValue(ctx, cb_result);
            JS_ThrowTypeError(ctx, "constructor Response requires 'new'");
            return -1;
        }
        s->stage = entry == RESP_ENTRY_JSON ? RESP_SERIALIZE : RESP_INIT;
    }

    if (s->stage == RESP_SERIALIZE) {
        /* §6.4 json() step 1: "serialize a JavaScript value to JSON bytes", which is JSON.stringify — the
           page's `toJSON`, its getters and its Proxy traps, all of it. This engine deleted the C entry to that
           algorithm on purpose, because a C entry beside the step machine would be a second implementation of
           it; the way to run it is the way a page does, as a CALL REQUEST on the tramp where a loop inside a
           `toJSON` suspends like any other. */
        r = step_call_run(ctx, &s->cphase, STEP_CB(s->cb), response_json_stringify(), JS_UNDEFINED,
                          argc > 0 ? 1 : 0, argc > 0 ? &argv[0] : NULL, cb_result, &s->json,
                          out_cb, out_argc);
        if (r > 0) return r;          /* parked INSIDE the page's toJSON */
        if (r < 0) return -1;
        cb_result = JS_UNDEFINED;
        /* stringify answers `undefined` for a value with no JSON form — a function, an undefined, a symbol —
           and §6.4 makes that a TypeError rather than an empty body. */
        if (JS_IsUndefined(s->json)) {
            JS_ThrowTypeError(ctx, "the value has no JSON representation");
            return -1;
        }
        s->stage = RESP_INIT;
    }

    if (s->stage == RESP_INIT) {
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
        s->stage = RESP_HEADERS;
    }

    if (s->stage == RESP_HEADERS) {
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
        s->stage = RESP_BODY;
    }

    DCHECK(s->stage == RESP_BODY, "the Response constructor was re-entered at a stage it never parks in");
    JS_FreeValue(ctx, cb_result);
    d = response_of(s->result);
    /* §6.4 step 8: a non-null body. §5.1's extraction is body.c's — one implementation of the union for both
       including interfaces — so what is left here is the two things §6.4 itself does: refuse a body on a
       null-body status, and set Content-Type when the extraction produced one and the header list has none. */
    if (entry == RESP_ENTRY_JSON || (!JS_IsNull(body) && !JS_IsUndefined(body))) {
        char *mime = NULL;

        if (response_is_null_body_status(d->status)) {
            JS_ThrowTypeError(ctx, "a Response with a null body status cannot be given a body");
            return -1;
        }
        /* THE TWO SOURCES A BODY COMES FROM, and the only place the two entries differ. The constructor runs
           §5.1's extraction over the BodyInit union, which decides both the bytes and the type; json() has the
           bytes already and §6.4 fixes the type at application/json. Everything after this — the null-body
           refusal above and the Content-Type rule below — is the same for both, which is why they are one
           machine. */
        if (entry == RESP_ENTRY_JSON) {
            size_t len = 0;
            const char *bytes = JS_ToCStringLen(ctx, &len, s->json);
            int bad;
            if (!bytes) return -1;
            bad = body_state_set(ctx, &d->body, bytes, len) < 0;
            JS_FreeCString(ctx, bytes);
            if (bad) return -1;
            mime = strdup("application/json");
            CHECK(mime, "response: OOM naming a JSON body's type");
        } else if (body_extract(ctx, &d->body, body, &mime) < 0) {
            free(mime);
            return -1;
        }
        /* §6.4 step 8.2: the extracted body's type is set ONLY when the header list does not already carry
           one — an init that named a Content-Type wins over the arm's default. */
        if (mime) {
            const HeaderList *hl = headers_list_of(d->headers);
            char *existing = header_list_get(hl, "content-type");
            if (!existing)
                header_list_append((HeaderList *)hl, "content-type", mime);
            free(existing);
            free(mime);
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

/* §6.4 redirect(url, status). It runs none of the page's code — the declaration converted the URL to a
   USVString and the status to an unsigned short before this was entered — so it is an ordinary member. Its two
   failures are DIFFERENT errors and the spec means them to be: an unparseable URL is a TypeError, a status
   outside the five redirect statuses is a RangeError, and that is how a page tells a bad address from a bad
   status. */
static JSValue js_response_redirect(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                                    int magic)
{
    UrlRecord rec;
    ResponseData *d;
    HeaderList hl = { 0 };
    JSValue obj;
    const char *url;
    size_t len = 0;
    int64_t status = 302;
    char *serialized;
    bool ok;

    (void)this_val; (void)magic;
    url = JS_ToCStringLen(ctx, &len, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (!url) return JS_EXCEPTION;
    if (argc > 1 && !JS_IsUndefined(argv[1]) && JS_ToInt64(ctx, &status, argv[1]) < 0) {
        JS_FreeCString(ctx, url);
        return JS_EXCEPTION;
    }
    ok = fetch_parse_url(&rec, url, len);
    JS_FreeCString(ctx, url);
    if (!ok) {
        url_record_free(&rec);
        return JS_ThrowTypeError(ctx, "the redirect URL could not be parsed");
    }
    if (status != 301 && status != 302 && status != 303 && status != 307 && status != 308) {
        url_record_free(&rec);
        return JS_ThrowRangeError(ctx, "%d is not a redirect status", (int)status);
    }
    serialized = url_serialize(&rec, false);
    url_record_free(&rec);
    obj = response_alloc(ctx, &d);
    if (JS_IsException(obj)) { free(serialized); return obj; }
    /* §6.4: the response's header list carries Location and its guard is "immutable", so the header is
       appended to the LIST the response is built with — going through the Headers interface afterwards would
       be refused by the guard the same step sets. */
    header_list_append(&hl, "location", serialized);
    free(serialized);
    ok = response_set(ctx, d, "", (int)status, "", RESPONSE_TYPE_DEFAULT, NULL, 0, &hl,
                      HEADERS_GUARD_IMMUTABLE) == 0;
    header_list_free(&hl);
    if (!ok) { JS_FreeValue(ctx, obj); return JS_EXCEPTION; }
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
    /* json()'s `data` is `any` — it crosses unconverted, because JSON.stringify is what looks at it. */
    static const IdlArgType JSON_ARGS[2] = { IDL_ANY, IDL_DICT };
    static const IdlArgType REDIRECT_ARGS[2] = { IDL_USVSTRING, IDL_UNSIGNED_SHORT };
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
    g_body_handle = body_declare(ctx, g_response_class, response_body_of, response_body_mime, "Response");
    /* ONE PROTOTYPE, not a copy of the members per instance. They were own properties of each Response, which
       is not what Web IDL describes and is observable three ways: `Response.prototype.text` was absent,
       `delete r.text` removed the method, and every reply paid for eight property installs. */
    g_response_proto = JS_NewObject(ctx);
    CHECK(!JS_IsException(g_response_proto), "Response.prototype could not be allocated");
    idl_interface_tag(ctx, g_response_proto, "Response");
    JS_SetPropertyFunctionList(ctx, g_response_proto, js_response_proto_funcs,
                               (int)(sizeof(js_response_proto_funcs) / sizeof(js_response_proto_funcs[0])));
    /* §5.2's mixin: the four readers and `bodyUsed`, from the one component Request includes too. */
    body_install(ctx, g_response_proto, g_body_handle);
    /* §6.4's clone(), a STEP because cloning a body tees its stream — see js_response_clone_step. */
    idl_install_method(ctx, g_response_proto, "clone", 0,
                       idl_method_id_step(ctx, NULL, 0, NULL, 0, &js_response_clone_decl, 0));
    g_ctor_stepid = idl_method_id_step(ctx, CTOR_ARGS, 2, RESPONSE_INIT, 3, &js_response_ctor_decl,
                                       RESP_ENTRY_CTOR);
    idl_optional_from(0);   /* §6.4: `constructor(optional BodyInit? body = null, optional ResponseInit init = {})` */
    /* §6.4's `static Response json(any data, optional ResponseInit init = {})` — the SAME machine under the
       other entry, so "initialize a response" has one implementation. */
    g_json_stepid = idl_method_id_step(ctx, JSON_ARGS, 2, RESPONSE_INIT, 3, &js_response_ctor_decl,
                                       RESP_ENTRY_JSON);
    idl_optional_from(1);   /* §6.4: `data` is required, `init` is not */
    g_redirect_stepid = idl_method_id(ctx, REDIRECT_ARGS, 2, js_response_redirect, 0);
    idl_optional_from(1);   /* §6.4: `redirect(USVString url, optional unsigned short status = 302)` */
    {
        JSValue global = JS_GetGlobalObject(ctx);
        JSValue json = JS_GetPropertyStr(ctx, global, "JSON");
        g_json_stringify = JS_GetPropertyStr(ctx, json, "stringify");
        CHECK(JS_IsFunction(ctx, g_json_stringify),
              "%JSON.stringify% is not a function — Response.json has no serializer to run");
        JS_FreeValue(ctx, json);
        JS_FreeValue(ctx, global);
    }
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
    /* The two statics whose arguments the args machine converts, installed on the INTERFACE OBJECT — which is
       what `static` means in the IDL and is the only difference from a prototype member. */
    idl_install_method(ctx, ctor, "json", 1, g_json_stepid);
    idl_install_method(ctx, ctor, "redirect", 1, g_redirect_stepid);
    JS_SetPropertyStr(ctx, (JSValue)global, "Response", ctor);
}

void response_free(JSContext *ctx)
{
    JS_FreeValue(ctx, g_response_proto);
    JS_FreeValue(ctx, g_json_stringify);
    g_response_proto = g_json_stringify = JS_UNDEFINED;
    g_response_rt = NULL;
    g_ctor_stepid = g_json_stepid = g_redirect_stepid = -1;
}

/* THE REPLY THE TRUSTED HOST FETCHED — type "basic", status 200, and headers the page may not write, which is
   the §6.4 "immutable" guard and the reason that guard exists at all. */
JSValue response_new(JSContext *ctx, const char *url, int status, const char *status_text,
                     const HeaderList *headers, const char *body, size_t body_len)
{
    ResponseData *d;
    JSValue obj = response_alloc(ctx, &d);

    if (JS_IsException(obj))
        return obj;
    /* THE REPLY'S OWN status and headers, not this component's guesses. They were 200/"OK"/none, which is what
       a host that could only hand over bytes forced — and a page reading `r.status` or `r.headers.get(...)` off
       a real reply got an answer this file invented. The guard is §6.4's "immutable": a reply the page did not
       construct is not a reply the page may rewrite. */
    if (response_set(ctx, d, url, status, status_text ? status_text : "", RESPONSE_TYPE_BASIC,
                     body ? body : "", body_len, headers, HEADERS_GUARD_IMMUTABLE) < 0) {
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    return obj;
}
