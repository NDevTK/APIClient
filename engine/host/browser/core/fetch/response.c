/* THE RESPONSE INTERFACE — WHATWG Fetch §5.5 "Response class", over the body the trusted host supplied.
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
#include "core/fetch/reply_source.h"   /* the ONE spelling of a reply's source identity — shared with XHR */
#include "core/idl_args.h"
#include "core/realm.h"
#include "core/fetch/body.h"
#include "core/streams/readable_stream.h"

static JSClassID g_response_class;
static int       g_ctor_stepid = -1, g_json_stepid = -1, g_redirect_stepid = -1, g_clone_stepid = -1;
static int       g_body_handle = -1;
/* One WASM instance is one document, so the class id and the body readers' step ids below are that runtime's;
   the DCHECK at install is what makes a second one impossible rather than merely unlikely. */
static JSRuntime *g_response_rt;
/* %JSON.stringify%, captured per realm at that realm's install. §5.5's "serialize a JavaScript value to JSON
   bytes" is the ABSTRACT operation, not a property read — a page that replaces `JSON.stringify` does not
   thereby change what `Response.json` does — so the original function object is held rather than looked up per
   call. It is read before any page script runs, off the engine's own builtin, so the capture itself runs
   nothing of the page's.
   IT IS PER REALM because §3.7 says so and because the value is a function object, which carries the realm it
   was minted in: one module static handed every document the FIRST realm's serializer, and a serializer runs
   the page's `toJSON` — so the callback flows of a child document ran in the parent's realm. */
static int g_json_stringify_slot = -1;

/* OWNED: the caller frees. */
static JSValue response_json_stringify(JSContext *ctx)
{
    DCHECK(g_json_stringify_slot > 0, "Response.json ran before response_init declared its %JSON.stringify% slot");
    return realm_value_get(ctx, g_json_stringify_slot);
}

/* THE BODY IS BYTES AND CARRIES ITS LENGTH. A body stream is a byte sequence in the spec, and it was a C string
   here: a reply with an interior NUL truncated at it silently, which `arrayBuffer()` and `bytes()` — the two
   readers whose entire job is to hand those bytes back — would then have reported as the whole body. There is
   no length a strlen can recover once the buffer is copied, so the length rides the record from the call that
   builds it, and `text()`/`json()` read exactly as far. */
/* §5.5's RESPONSE, as the spec's fields and not as constants. `ok`/`status`/`statusText`/`type` were literals
   here — 200, "OK", "basic" — which was right for the one Response this component could build (the reply the
   trusted host had already fetched) and wrong the moment the page can build one: `new Response("", {status:
   404})` is not 200, and every `if (r.ok)` in a page that constructs its own responses forked the wrong way.
   The BODY is §5.3's mixin, whose one implementation Request includes too. */
/* §2.2.6's URL LIST, AND WHY IT IS A JS ARRAY.
 *
 * "A response has an associated URL list (a list of zero or more URLs). Unless stated otherwise, it is « »."
 * and "A response has an associated URL. It is a pointer to the last URL in response's URL list and null if
 * response's URL list is empty." There was ONE `char *url` here, so every question that is really about the
 * LIST had to be answered by something else: `redirected` — §5.5's "The redirected getter steps are to return
 * true if this's response's URL list's size is greater than 1; otherwise false." — was the literal `false`,
 * which is not a value this file computed but a value it asserted, and a bundle's `if (r.redirected)` never
 * forked the arm that handles one.
 *
 * IT IS A JS VALUE for the reason CLAUDE.md §State-isolation gives: a list that must PARK to the cold tier and
 * FORK per-flow gets both for free from a JS Array — the snapshot machinery already carries it, and the
 * runtime's own leak walk (the gc_obj_list pass the gates count leaks with) can SEE it, which it cannot do for
 * a `char **`. It is also the shape `headers` beside it already is, so the record has ONE ownership rule for
 * its owned values rather than two. It is not mutated after the response is built — a fetch grows the list
 * (§4.5 "Append locationURL to request's URL list") and hands it over complete — so nothing here has a write
 * site to capture; it forks and parks as a value, which is what the rule is about.
 *
 * ITS ITEMS ARE SERIALIZED URLS, because that is what crosses the host bridge: a live URL record crosses
 * neither an instance, nor a session, nor a park. `url` runs the real parser back over the last one to answer
 * "serialized with exclude fragment set to true". */
typedef struct {
    JSValue url_list;         /* §2.2.6's URL list — an Array of serialized URLs; « » is the empty Array */
    BodyState body;           /* §5.3's mixin, one implementation shared with Request */
    int     status;
    char   *status_text;      /* the response's "status message" */
    uint8_t type;             /* RESPONSE_TYPE_* */
    /* [SameObject]: §5.5 declares `headers` SameObject, so the Headers OBJECT is built once and held here
       rather than minted per read — a page that does `const h = r.headers; h.set(...)` must be writing into
       the response's own list, and `r.headers === r.headers` is asserted by wpt directly. */
    JSValue headers;
} ResponseData;

/* §5.5's response types. "default" is what the CONSTRUCTOR makes; "basic" is a same-origin reply the host
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
        JS_FreeValueRT(rt, d->url_list);
        body_state_free(rt, &d->body);
        js_free_rt(rt, d->status_text); js_free_rt(rt, d);
    }
}

/* The Headers object and the URL list a Response holds are JSValues in the class opaque, which the collector
   cannot see through — and a Headers can reach back (nothing today, but the cycle is the collector's problem to
   be told about, not the component's to promise will not form). This list is the SAME list the finalizer above
   frees, for the reason CLAUDE.md §Architecture gives: a field added to one and not the other is caught by
   reading the two together. */
static void response_gc_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    ResponseData *d = JS_GetOpaque(val, g_response_class);
    if (d) JS_MarkValue(rt, d->headers, mark_func);
    if (d) JS_MarkValue(rt, d->url_list, mark_func);
    if (d) body_state_mark(rt, &d->body, mark_func);
}

static ResponseData *response_of(JSValueConst v) { return JS_GetOpaque(v, g_response_class); }

/* How §5.3's shared readers find this interface's body. */
/* §5.3's `formData()` asks the including interface for its Content-Type, because only it knows where its
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
static int response_set(JSContext *ctx, ResponseData *d, JSValueConst url_list, int status,
                        const char *status_text, int type, const char *body, size_t body_len,
                        const HeaderList *headers, HeadersGuard guard);

/* §2.2.6's URL list, from the SERIALIZED URLs a caller observed. `n == 0` is the spec's « ». Every item is an
   ABSOLUTE URL: §5.4's constructor parses a request's URL before it fetches and §4.5 parses a redirect's
   Location against it, so a relative reference or a concolic display shape reaching here is a host that
   reported something that is not a URL, and `url` below would then have nothing to serialize.
   THE ITEMS ARE OWN DEFINES, NEVER A [[Set]], for the reason fetch.c's fetch_reply_new states in full at its
   own writes: this list is part of a record the TRUSTED ZONE builds, its array's prototype is the PAGE'S
   `Array.prototype`, and [[Set]] (ECMA-262 §10.1.9 "[[Set]] ( propertyKey, value, receiver )") walks that chain — so an index
   accessor the page installed there would be CALLED while the host is building the record, and a frozen
   Array.prototype would make the write refuse by throwing into the host's own time. A define creates an own
   data property and asks the chain nothing. */
JSValue response_url_list(JSContext *ctx, const char *const *urls, int n)
{
    JSValue a = JS_NewArray(ctx);
    int i;

    DCHECK(n >= 0, "a response's URL list was asked for with a negative size");
    if (JS_IsException(a))
        return a;
    for (i = 0; i < n; i++) {
        DCHECK(urls[i] != NULL,
               "a response's URL list was built with a hole in it — a host reports every item of the list it "
               "observed, or none of them, and a missing item silently shortens the list `redirected` is read "
               "off");
        if (JS_DefinePropertyValueUint32(ctx, a, (uint32_t)i, JS_NewString(ctx, urls[i]),
                                         JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, a);
            return JS_EXCEPTION;
        }
    }
    return a;
}

/* A COPY of a URL list, which is what §2.2.6's "clone a response" — "Let newResponse be a copy of response,
   except for its body", the operation §5.5's clone() step 2 performs — takes. The items are immutable strings,
   so the copy is a reference each; the ARRAY is new because two responses sharing one would be one list
   wearing two names. */
static JSValue response_url_list_copy(JSContext *ctx, JSValueConst src)
{
    JSValue out = JS_NewArray(ctx);
    int64_t n = 0, i;

    if (JS_IsException(out) || JS_IsUndefined(src))
        return out;                        /* JS_UNDEFINED is how a caller spells the spec's « » */
    if (JS_GetLength(ctx, src, &n) < 0) { JS_FreeValue(ctx, out); return JS_EXCEPTION; }
    for (i = 0; i < n; i++) {
        JSValue u = JS_GetPropertyUint32(ctx, src, (uint32_t)i);
        if (JS_IsException(u)) { JS_FreeValue(ctx, out); return JS_EXCEPTION; }
        DCHECK(JS_IsString(u), "a response's URL list held something that is not a serialized URL");
        if (JS_SetPropertyUint32(ctx, out, (uint32_t)i, u) < 0) {
            JS_FreeValue(ctx, out);
            return JS_EXCEPTION;
        }
    }
    return out;
}

/* §2.2.6: "A response has an associated URL. It is a pointer to the last URL in response's URL list and null if
   response's URL list is empty." — the ONE place that reads the list's end, so `url` and anything after it
   agree by construction. Returns the list's size through `*psize` because §5.5's `redirected` is the SAME
   read ("this's response's URL list's size is greater than 1") and a second walk could disagree with this one.
   The returned value is JS_NULL for the null URL, otherwise the owned serialized-URL string. */
static JSValue response_url_of(JSContext *ctx, ResponseData *d, int64_t *psize)
{
    int64_t n = 0;

    DCHECK(JS_IsArray(d->url_list), "a Response reached its URL list before response_alloc built one");
    if (JS_GetLength(ctx, d->url_list, &n) < 0)
        return JS_EXCEPTION;
    *psize = n;
    if (n == 0)
        return JS_NULL;
    return JS_GetPropertyUint32(ctx, d->url_list, (uint32_t)(n - 1));
}

/* §5.5's `url`, AS THE C STRING BOTH ITS READERS NEED: "this's response's URL, serialized with exclude
   fragment set to true". The exclusion is the URL PARSER'S — the list holds serialized URLs, so the parser is
   run back over the last one and asked to serialize it again without its fragment, rather than this file
   deciding where a fragment starts.
   IT IS ONE ANSWER BECAUSE THERE ARE TWO READERS. The `url` getter is one; the body's SOURCE IDENTITY
   (core/fetch/reply_source.h) is the other, and a reply NAMED with a fragment the getter reports WITHOUT is
   one reply wearing two spellings — every predicate over the value read under one of them deciding nothing
   about the other. A fragment is also never sent, so two addresses differing only there are one reply.
   Returns MALLOC'D, or NULL for §2.2.6's null URL — which is a statement (this response has no address),
   never a hole. */
static char *response_url_serialized(JSContext *ctx, ResponseData *d)
{
    int64_t n = 0;
    JSValue last = response_url_of(ctx, d, &n);
    const char *s;
    size_t len = 0;
    UrlRecord rec;
    char *ser;
    bool ok;

    CHECK(!JS_IsException(last), "response: OOM reading the end of a response's URL list");
    if (JS_IsNull(last))
        return NULL;                                  /* the response's URL is null */
    s = JS_ToCStringLen(ctx, &len, last);
    JS_FreeValue(ctx, last);
    CHECK(s != NULL, "response: OOM reading a response's URL");
    url_record_init(&rec);
    /* NO BASE, because every item of a response's URL list is ABSOLUTE by the time it is in one. */
    ok = url_parse(&rec, s, len, NULL);
    JS_FreeCString(ctx, s);
    DCHECK(ok, "a response's URL list held a string the URL parser refuses — §2.2.6's list is a list of URLs, "
               "so the host that filled it reported a relative reference, a concolic display shape or an empty "
               "string where a serialized absolute URL belongs");
    if (!ok) {
        /* A response whose address is not a URL has no address, and §5.5 already spells what that answers:
           the empty string. Saying so is the same statement the null-URL arm above makes. */
        url_record_free(&rec);
        return NULL;
    }
    ser = url_serialize(&rec, /*exclude_fragment*/ true);
    url_record_free(&rec);
    CHECK(ser != NULL, "response: OOM serializing the response's URL");
    return ser;
}

/* WHERE A Response'S BYTES CAME FROM — core/byte_reader.h's question, reaching this interface through §5.3's
   mixin. A Response the HOST fetched carries the address the server answered at; one the PAGE constructed
   (`new Response(JSON.stringify(x))`) has an empty URL list, so its bytes are the page's own and this answers
   the NULL that says so. The spelling is core/fetch/reply_source.h's, shared with XMLHttpRequest, because the
   two are two doors onto one fact. */
static char *response_body_source(JSContext *ctx, JSValueConst v)
{
    ResponseData *d = JS_GetOpaque(v, g_response_class);
    char *url, *name;

    DCHECK(d != NULL, "a Response was asked where its bytes came from and is not a Response — the mixin's own "
                      "receiver check has already answered for that, so a NULL here is its `of` and this "
                      "interface's opaque disagreeing");
    if (!d)
        return NULL;
    url = response_url_serialized(ctx, d);
    if (!url)
        return NULL;
    name = reply_source_name(url, strlen(url));
    free(url);
    return name;
}

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
/* IT IS A STEP, because §2.2.4's "clone a body" is a TEE and a tee is a call of code the page can reach. The two
   synchronous refusals still happen where the page wrote its try/catch: a locked or disturbed body is a
   TypeError from clone() itself, not a rejected promise, because clone is not a body-consuming method. */
typedef struct {
    uint8_t phase;
    JSValue obj;       /* the clone being built (owned) */
    JSValue cb[2];     /* step_call_run's buffer: [this, tee] — the tee takes no arguments */
} JSCloneState;

/* WHERE THIS MACHINE RESTS. §5.5's clone() is three steps and the page's code runs in exactly one of them:
   step 2's "clone a response" performs §2.2.4's "clone a body", which is a TEE — a call. */
#define RESP_CLONE_STAGES(X) \
    X(CL_ENTRY = IDL_STEP_FIRST, \
      "Fetch §5.5 clone() steps 1-2 (the unusable refusal, then §2.2.6's clone a response down to its body)") \
    X(CL_BODY, "Fetch §2.2.4 clone a body step 1 (tee this's body's stream)") \
    X(CL_DONE, "Fetch §5.5 clone() step 3 (the Response object for the cloned response)")
enum { RESP_CLONE_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const RESP_CLONE_STEPS[] = { RESP_CLONE_STAGES(JS_STEP_STAGE_LABEL) NULL };

static void js_clone_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSCloneState *s = st;
    int k;
    v->val(ctx, &s->obj);
    for (k = 0; k < 2; k++) v->val(ctx, &s->cb[k]);
}

static int js_response_clone_step(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                                  JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    JSCloneState *s = st;
    ResponseData *d = response_of(hdr->this_val), *c;
    int r;

    (void)argc; (void)argv;
    if (hdr->stage == CL_ENTRY) {
        bool locked = false;
        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        s->obj = JS_UNDEFINED;
        s->cb[0] = s->cb[1] = JS_UNDEFINED;
        if (!d)
            return JS_ThrowTypeError(ctx, "not a Response"), -1;
        /* §5.3's "unusable" is DISTURBED **or** LOCKED — a body whose stream a page has taken a reader on
           cannot be teed, and reporting only the read latch let clone() reach the tee and throw from inside
           it, which is the wrong error at the wrong place. */
        readable_stream_query(d->body.stream, NULL, &locked);
        if (d->body.used || readable_stream_disturbed(d->body.stream) || locked)
            return JS_ThrowTypeError(ctx, "cannot clone a Response whose body is disturbed or locked"), -1;
        s->obj = response_alloc(ctx, &c);
        if (JS_IsException(s->obj)) { s->obj = JS_UNDEFINED; return -1; }
        /* §5.5 clone COPIES the response, so every field goes — a clone that kept only the body reported status
           200 and type "basic" for a 404 the page had just constructed. The headers are a NEW Headers over the
           same list, because [SameObject] is per response and `r.clone().headers === r.headers` is false. The
           BODY is not among them: it is the tee below, not a copy. */
        if (response_set(ctx, c, d->url_list, d->status, d->status_text, d->type, NULL, 0,
                         headers_list_of(d->headers), headers_guard_of(d->headers)) < 0)
            return -1;
        STEP_GOTO(hdr->stage, CL_BODY, &s->phase, NULL);
    }

    DCHECK(hdr->stage == CL_BODY, "Response.clone resumed in a stage §5.5 does not have");
    DCHECK(d != NULL, "a Response stopped being one between two stages of its own clone");
    c = response_of(s->obj);
    DCHECK(c != NULL, "the clone this machine allocated stopped being a Response");
    r = body_clone_run(ctx, &s->phase, s->cb, 2, &c->body, &d->body, cb_result, out_cb, out_argc);
    if (r > 0) return r;
    if (r < 0) return -1;
    STEP_GOTO(hdr->stage, CL_DONE, &s->phase, NULL);
    *presult = s->obj;
    s->obj = JS_UNDEFINED;
    return 0;
}

static const IdlStepDecl js_response_clone_decl = {
    js_response_clone_step, sizeof(JSCloneState), js_clone_visit, NULL,
    "Fetch §5.5 Response.clone()", RESP_CLONE_STEPS
};

static JSValue js_response_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    ResponseData *d = response_of(this_val);
    if (!d)
        return JS_ThrowTypeError(ctx, "not a Response");
    switch (magic) {
    /* §5.5 `ok` IS the 200..299 test, not a flag: a page's `if (r.ok)` and its `if (r.status < 400)` must
       agree, and they only do when both read the same number. */
    case 0: return JS_NewBool(ctx, d->status >= 200 && d->status <= 299);
    case 1: return JS_NewInt32(ctx, d->status);
    case 2: return JS_NewString(ctx, d->status_text ? d->status_text : "");
    /* §5.5 `url`: "return the empty string if this's response's URL is null; otherwise this's response's URL,
       serialized with exclude fragment set to true" — response_url_serialized performs the whole of that, and
       is the SAME answer the body's source identity is named from. */
    case 3: {
        char *ser = response_url_serialized(ctx, d);
        JSValue out;

        if (!ser) return JS_NewString(ctx, "");   /* the response's URL is null */
        out = JS_NewString(ctx, ser);
        free(ser);
        return out;
    }
    /* §5.5 `redirected`: "return true if this's response's URL list's size is greater than 1; otherwise
       false." It was the literal `false`, so a bundle's redirect-handling arm was never reached. */
    case 4: {
        int64_t n = 0;
        JSValue last = response_url_of(ctx, d, &n);
        if (JS_IsException(last)) return last;
        JS_FreeValue(ctx, last);
        return JS_NewBool(ctx, n > 1);
    }
    case 5: return JS_NewString(ctx, RESPONSE_TYPE_NAME[d->type]);
    default:
        DCHECK(magic == 6, "a Response accessor was declared with a magic this component does not answer");
        return JS_DupValue(ctx, d->headers);                                 /* [SameObject] */
    }
}

/* §5.5's ATTRIBUTES. The seven here are Response's own; `body`, `bodyUsed` and the six readers — `text`,
   `json`, `arrayBuffer`, `bytes`, `blob`, `formData` — are §5.3's Body mixin and are installed by body.c, from the
   one implementation Request includes too. This comment used to say `body`, `blob()` and `formData()` were
   ABSENT because "this engine has no ReadableStream" and no Blob and no FormData; all three exist (body.c's
   BODY_READERS and body_install, over §4's ReadableStream and File API's Blob), so the prose was telling the
   next reader to build what was already built. */
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
    {
        JSValue proto = JS_GetClassProto(ctx, g_response_class);
        DCHECK(!JS_IsNull(proto), "a Response was minted in a realm that never ran its install");
        obj = JS_NewObjectProtoClass(ctx, proto, g_response_class);
        JS_FreeValue(ctx, proto);
    }
    if (JS_IsException(obj))
        return obj;
    d = js_mallocz(ctx, sizeof(*d));
    if (!d) { JS_FreeValue(ctx, obj); return JS_EXCEPTION; }
    d->headers = JS_UNDEFINED;
    /* §2.2.6's "Unless stated otherwise, it is « »" — a new response HAS a URL list, an empty one, so it is
       built HERE rather than left as a tag the getters would have to test for. The zeroed record's JSValue is
       the integer 0 and not JS_UNDEFINED, which is the trap this file has paid for elsewhere; an Array is a
       real answer to `url` and to `redirected` from the first instant the object exists. */
    d->url_list = JS_NewArray(ctx);
    d->status = 200;
    JS_SetOpaque(obj, d);
    if (JS_IsException(d->url_list)) { d->url_list = JS_UNDEFINED; JS_FreeValue(ctx, obj); return JS_EXCEPTION; }
    *pd = d;
    return obj;
}

/* Fill a freshly allocated response. `body` NULL is the spec's NULL BODY, which is not the empty body.
   `url_list` is §2.2.6's URL list and is COPIED, never adopted — JS_UNDEFINED spells the spec's « », which is
   what §5.5's constructor, error() and redirect() each create ("a new response"). Returns -1 with an exception
   live. */
static int response_set(JSContext *ctx, ResponseData *d, JSValueConst url_list, int status,
                        const char *status_text, int type, const char *body, size_t body_len,
                        const HeaderList *headers, HeadersGuard guard)
{
    JS_FreeValue(ctx, d->url_list);
    d->url_list = response_url_list_copy(ctx, url_list);
    if (JS_IsException(d->url_list)) { d->url_list = JS_UNDEFINED; return -1; }
    d->status_text = js_strdup(ctx, status_text ? status_text : "");
    if (!d->status_text) return -1;
    d->status = status;
    d->type = (uint8_t)type;
    if (body_state_set(ctx, &d->body, body, body_len) < 0) return -1;
    JS_FreeValue(ctx, d->headers);
    d->headers = headers_new(ctx, headers, guard);
    return JS_IsException(d->headers) ? -1 : 0;
}

/* ---- §5.5's constructor ----------------------------------------------------------------------------------
 *
 * A MACHINE, because both of its arguments run the page's code: the `body` is a `BodyInit?` union whose
 * USVString arm is a ToString the page may define, and `init` is a dictionary whose three members are three
 * [[Get]]s — one Proxy trap away each — followed by the header fill, which is the whole iterator or record
 * protocol over an object the page owns. The declaration converts both; what is left here is §5.5's own steps,
 * in the order the spec writes them. */
/* ONE MACHINE FOR THE CONSTRUCTOR AND FOR json(), because §5.5 gives them ONE algorithm: both create a
   response and then run "initialize a response" over the same init dictionary. They differ in exactly two
   places — where the BODY comes from, and whether `new` is required — and a second machine would have been a
   second copy of the status range check, the reason-phrase check, the header fill and the Content-Type rule.
   The magic says which entry this is. */
enum { RESP_ENTRY_CTOR = 0, RESP_ENTRY_JSON };
/* WHERE THIS MACHINE RESTS, ACROSS THE TWO ENTRIES THAT SHARE IT. §5.5's constructor is five steps whose fifth
   is §5.5's "initialize a response", and json() is five steps whose fourth is the same operation — so the
   stages after the entry are §5.5's, named by that algorithm rather than by whichever member reached it. The
   page's code runs at three of them: json()'s serialization, §5.5 step 5's fill, and nothing else.
   §5.5 step 4's EXTRACTION is performed at the body stage rather than before step 5, which is where the spec
   writes it: nothing between the two can throw or run the page's code, so the two orders are the same
   execution, and the label says where it actually happens. */
#define RESP_CTOR_STAGES(X) \
    X(RESP_START = IDL_STEP_FIRST, \
      "Fetch §5.5 new Response(body, init) steps 1-2 (a new response and its Headers; Web IDL §3.7.1's `new` " \
      "requirement precedes them, and Response.json() enters at the next step instead)") \
    X(RESP_SERIALIZE, \
      "Fetch §5.5 Response.json(data, init) step 1 (serialize a JavaScript value to JSON bytes — the page's " \
      "toJSON, getters and Proxy traps)") \
    X(RESP_INIT, \
      "Fetch §5.5 initialize a response steps 1-4 (the status range, the reason-phrase, and the response's " \
      "status and status message)") \
    X(RESP_HEADERS, \
      "Fetch §5.5 initialize a response step 5 (fill response's headers with init[\"headers\"])") \
    X(RESP_BODY, \
      "Fetch §5.5 initialize a response step 6 (§5.5 step 4's extraction, the null-body-status refusal, and " \
      "the Content-Type the extracted type contributes)")
enum { RESP_CTOR_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const RESP_CTOR_STEPS[] = { RESP_CTOR_STAGES(JS_STEP_STAGE_LABEL) NULL };

typedef struct {
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

/* The header list alone — see js_headers_ctor_release. The rest is js_response_ctor_visit's declaration. */
static void js_response_ctor_release(JSContext *ctx, void *st)
{
    (void)ctx;
    header_list_free(&((JSResponseCtorState *)st)->list);
}

/* §5.5 "a null body status": the statuses HTTP defines as carrying no body, which a constructed Response may
   therefore not be given one with. */
static int response_is_null_body_status(int status)
{
    return status == 101 || status == 103 || status == 204 || status == 205 || status == 304;
}

/* §5.5's `reason-phrase` production: HTAB, SP, VCHAR and obs-text — and nothing else, which is what makes
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

    if (hdr->stage == RESP_START) {
        /* `new` is the CONSTRUCTOR's requirement. json() is a static, so its receiver is the interface object
           and there is nothing to require. */
        if (entry == RESP_ENTRY_CTOR && JS_IsUndefined(hdr->this_val)) {
            JS_FreeValue(ctx, cb_result);
            JS_ThrowTypeError(ctx, "constructor Response requires 'new'");
            return -1;
        }
        STEP_GOTO(hdr->stage, entry == RESP_ENTRY_JSON ? RESP_SERIALIZE : RESP_INIT, &s->cphase, NULL);
    }

    if (hdr->stage == RESP_SERIALIZE) {
        /* §5.5 json() step 1: "serialize a JavaScript value to JSON bytes", which is JSON.stringify — the
           page's `toJSON`, its getters and its Proxy traps, all of it. This engine deleted the C entry to that
           algorithm on purpose, because a C entry beside the step machine would be a second implementation of
           it; the way to run it is the way a page does, as a CALL REQUEST on the tramp where a loop inside a
           `toJSON` suspends like any other. */
        {
            JSValue stringify = response_json_stringify(ctx);   /* THIS realm's, per §3.7 */
            r = step_call_run(ctx, &s->cphase, STEP_CB(s->cb), stringify, JS_UNDEFINED,
                              argc > 0 ? 1 : 0, argc > 0 ? &argv[0] : NULL, cb_result, &s->json,
                              out_cb, out_argc);
            JS_FreeValue(ctx, stringify);
        }
        if (r > 0) return r;          /* parked INSIDE the page's toJSON */
        if (r < 0) return -1;
        cb_result = JS_UNDEFINED;
        /* stringify answers `undefined` for a value with no JSON form — a function, an undefined, a symbol —
           and §5.5 makes that a TypeError rather than an empty body. */
        if (JS_IsUndefined(s->json)) {
            JS_ThrowTypeError(ctx, "the value has no JSON representation");
            return -1;
        }
        STEP_GOTO(hdr->stage, RESP_INIT, &s->cphase, NULL);
    }

    if (hdr->stage == RESP_INIT) {
        /* §5.5's initialize a response step 1: the RANGE check, after Web IDL's `unsigned short`
           conversion and never instead of it. */
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
        /* §5.5's initialize a response step 2: statusText must match reason-phrase. */
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
            /* §5.5's initialize a response steps 3-4: the response exists now — the constructor's step 1,
               json()'s step 3 — with its status and its status message. The BODY and
               the HEADERS are the two things still to come, and both may run the page's code. */
            s->result = response_alloc(ctx, &d);
            if (JS_IsException(s->result)) {
                JS_FreeCString(ctx, t);
                JS_FreeValue(ctx, v_text);
                JS_FreeValue(ctx, cb_result);
                return -1;
            }
            /* §5.5's `new Response(body, init)` step 1 creates "a new response", whose URL list is « » — so
               `url` answers "" (its URL is null) and `redirected` answers false, both computed from the list
               rather than declared. */
            r = response_set(ctx, d, JS_UNDEFINED, status, t ? t : "", RESPONSE_TYPE_DEFAULT, NULL, 0, NULL,
                             HEADERS_GUARD_RESPONSE);
            JS_FreeCString(ctx, t);
            JS_FreeValue(ctx, v_text);
            if (r < 0) { JS_FreeValue(ctx, cb_result); return -1; }
        }
        headers_fill_init(&s->fill);
        STEP_GOTO(hdr->stage, RESP_HEADERS, &s->cphase, NULL);
    }

    if (hdr->stage == RESP_HEADERS) {
        /* §5.5's initialize a response step 5: fill the response's headers, which have guard "response" — so
           a page that puts Set-Cookie on a Response it built silently gets nothing, which is the header wpt
           asserts by name. */
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
        STEP_GOTO(hdr->stage, RESP_BODY, &s->cphase, NULL);
    }

    DCHECK(hdr->stage == RESP_BODY,
           "the Response constructor was re-entered at a stage §5.5 and §5.5 do not have between them");
    JS_FreeValue(ctx, cb_result);
    d = response_of(s->result);
    /* §5.5's initialize a response step 6: a non-null body. §5.2's extraction is body.c's — one implementation
       of the union for both including interfaces — so what is left here is the two things step 6 itself
       does: refuse a body on a null-body status, and set Content-Type when the extraction produced one and
       the header list has none. */
    if (entry == RESP_ENTRY_JSON || (!JS_IsNull(body) && !JS_IsUndefined(body))) {
        char *mime = NULL;

        if (response_is_null_body_status(d->status)) {
            JS_ThrowTypeError(ctx, "a Response with a null body status cannot be given a body");
            return -1;
        }
        /* THE TWO SOURCES A BODY COMES FROM, and the only place the two entries differ. The constructor runs
           §5.2's extraction over the BodyInit union, which decides both the bytes and the type; json() has the
           bytes already and §5.5 fixes the type at application/json. Everything after this — the null-body
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
        /* §5.5's `new Response(body, init)` step 4 extracts with no keepalive flag at all — the flag is a
           REQUEST's, and a Response is not one, which is what §5.2's `= false` default says for a caller
           that names none. */
        } else if (body_extract(ctx, &d->body, body, /*keepalive*/ false, &mime) < 0) {
            free(mime);
            return -1;
        }
        /* §5.5's initialize a response step 6.3: the extracted body's type is set ONLY when the header list
           does not already carry one — an init that named a Content-Type wins over the arm's default. */
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

/* §5.5 error(): "a new response whose type is 'error'". It runs none of the page's code — no arguments, no
   init — so it is an ordinary member and not a machine. Its status is 0, which is outside the range the
   CONSTRUCTOR enforces, because that range is the constructor's step and not the response's. */
static JSValue js_response_error(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    ResponseData *d;
    JSValue obj = response_alloc(ctx, &d);
    (void)this_val; (void)argc; (void)argv;
    if (JS_IsException(obj))
        return obj;
    if (response_set(ctx, d, JS_UNDEFINED, 0, "", RESPONSE_TYPE_ERROR, NULL, 0, NULL,
                     HEADERS_GUARD_IMMUTABLE) < 0) {
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    return obj;
}

/* §5.5 redirect(url, status). It runs none of the page's code — the declaration converted the URL to a
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
    ok = fetch_parse_url(ctx, &rec, url, len);
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
    /* §5.5: the response's header list carries Location and its guard is "immutable", so the header is
       appended to the LIST the response is built with — going through the Headers interface afterwards would
       be refused by the guard the same step sets. */
    header_list_append(&hl, "location", serialized);
    free(serialized);
    /* §5.5's redirect() creates "a new response" and sets its STATUS and its `Location` header — and nothing
       else. Its URL list stays « », so `responseObject.url` is "" and `redirected` is false: the address the
       page passed is the redirect TARGET, which the list does not hold. */
    ok = response_set(ctx, d, JS_UNDEFINED, (int)status, "", RESPONSE_TYPE_DEFAULT, NULL, 0, &hl,
                      HEADERS_GUARD_IMMUTABLE) == 0;
    header_list_free(&hl);
    if (!ok) { JS_FreeValue(ctx, obj); return JS_EXCEPTION; }
    return obj;
}

static const JSCFunctionListEntry js_response_static_funcs[] = {
    JS_CFUNC_DEF("error", 0, js_response_error),
};

static const IdlStepDecl js_response_ctor_decl = {
    js_response_ctor_step, sizeof(JSResponseCtorState), js_response_ctor_visit, js_response_ctor_release,
    "Fetch §5.5 new Response(body, init) / Response.json(data, init), through §5.5 initialize a response",
    RESP_CTOR_STEPS
};

/* ---- install --------------------------------------------------------------------------------------------- */

void response_init(JSContext *ctx)
{
    JSClassDef def = { "Response", .finalizer = response_finalizer, .gc_mark = response_gc_mark };
    JSRuntime *rt = JS_GetRuntime(ctx);
    /* `optional BodyInit? body = null, optional ResponseInit init = {}`. The dictionary's members are listed in
       the order Web IDL reads them, which is lexicographic and not the order §5.5 uses them in. */
    static const IdlArgType CTOR_ARGS[2] = { IDL_BODYINIT_NULLABLE, IDL_DICT };
    /* json()'s `data` is `any` — it crosses unconverted, because JSON.stringify is what looks at it. */
    static const IdlArgType JSON_ARGS[2] = { IDL_ANY, IDL_DICT };
    static const IdlArgType REDIRECT_ARGS[2] = { IDL_USVSTRING, IDL_UNSIGNED_SHORT };
    static const IdlDictMember RESPONSE_INIT[3] = {
        { "headers",    IDL_ANY,            false },   /* HeadersInit: the union the fill converts */
        { "status",     IDL_UNSIGNED_SHORT, false },
        { "statusText", IDL_BYTESTRING,     false },   /* the reason-phrase check on top is §5.5's */
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
    g_body_handle = body_declare(ctx, g_response_class, response_body_of, response_body_mime, response_body_source,
                                 "Response");
    g_clone_stepid = idl_method_id_step(ctx, NULL, 0, NULL, 0, &js_response_clone_decl, 0);
    g_ctor_stepid = idl_method_id_step(ctx, CTOR_ARGS, 2, RESPONSE_INIT, 3, &js_response_ctor_decl,
                                       RESP_ENTRY_CTOR);
    idl_optional_from(0);   /* §5.5: `constructor(optional BodyInit? body = null, optional ResponseInit init = {})` */
    /* §5.5's `static Response json(any data, optional ResponseInit init = {})` — the SAME machine under the
       other entry, so "initialize a response" has one implementation. */
    g_json_stepid = idl_method_id_step(ctx, JSON_ARGS, 2, RESPONSE_INIT, 3, &js_response_ctor_decl,
                                       RESP_ENTRY_JSON);
    idl_optional_from(1);   /* §5.5: `data` is required, `init` is not */
    g_redirect_stepid = idl_method_id(ctx, REDIRECT_ARGS, 2, js_response_redirect, 0);
    idl_optional_from(1);   /* §5.5: `redirect(USVString url, optional unsigned short status = 302)` */
    g_json_stringify_slot = realm_value_declare(ctx, "%JSON.stringify% (Response.json)");
    realm_declare_intrinsic(response_install_proto);
}

/* FETCH §5.5 "Response class"' INTERFACE PROTOTYPE OBJECT, ITS SERIALIZER *AND* ITS INTERFACE OBJECT, FOR ONE
   REALM.
   ONE PROTOTYPE PER REALM, not a copy of the members per instance. They were own properties of each Response,
   which is not what Web IDL describes and is observable three ways: `Response.prototype.text` was absent,
   `delete r.text` removed the method, and every reply paid for eight property installs.
   THE INTERFACE OBJECT IS MINTED HERE AND NOT FROM A PER-DOCUMENT INSTALL. §5.5 declares
   `[Exposed=(Window,Worker)] interface Response`, and Web IDL §3.8 "Platform objects implementing interfaces"'
   `define the global property references` is "To define the global property references on target, given realm
   realm" whose step 1 is "Let interfaces be a list that contains every interface that is exposed in realm" —
   a REALM, with no Document in the algorithm, so a realm that reaches no platform_document_install got no
   `Response`. §5.5's TWO STATICS COME WITH IT: `json` and `redirect` are members of the INTERFACE OBJECT
   rather than of the prototype, so an interface object minted without them is a `Response` a page can
   feature-detect and not call — which is why they moved in the same edit rather than being left behind. */
void response_install_proto(JSContext *ctx)
{
    JSValue proto, prev, ctor;

    DCHECK(g_response_class != 0, "a realm asked for Response.prototype before the class was declared");
    DCHECK(g_ctor_stepid >= 0, "a realm asked for Response before response_init declared its constructor");
    prev = JS_GetClassProto(ctx, g_response_class);
    DCHECK(JS_IsNull(prev), "response_install_proto ran twice in one realm");
    JS_FreeValue(ctx, prev);

    proto = JS_NewObject(ctx);
    CHECK(!JS_IsException(proto), "Response.prototype could not be allocated");
    idl_interface_tag(ctx, proto, "Response");
    JS_SetPropertyFunctionList(ctx, proto, js_response_proto_funcs,
                               (int)(sizeof(js_response_proto_funcs) / sizeof(js_response_proto_funcs[0])));
    /* §5.3's mixin: the four readers and `bodyUsed`, from the one component Request includes too. */
    body_install(ctx, proto, g_body_handle);
    /* §5.5's clone(), a STEP because cloning a body tees its stream — see js_response_clone_step. */
    idl_install_method(ctx, proto, "clone", g_clone_stepid);

    ctor = idl_step_constructor(ctx, "Response", g_ctor_stepid);
    CHECK(!JS_IsException(ctor), "the Response interface object could not be allocated");
    JS_SetConstructor(ctx, ctor, proto);
    JS_SetPropertyFunctionList(ctx, ctor, js_response_static_funcs,
                               (int)(sizeof(js_response_static_funcs) / sizeof(js_response_static_funcs[0])));
    /* The two statics whose arguments the args machine converts, installed on the INTERFACE OBJECT — which is
       what `static` means in the IDL and is the only difference from a prototype member. */
    idl_install_method(ctx, ctor, "json", g_json_stepid);
    idl_install_method(ctx, ctor, "redirect", g_redirect_stepid);
    /* THE HANDOVER IS LAST: JS_SetClassProto TAKES the reference, so `proto` is this function's until the realm
       owns it, and the Web IDL §3.7.1 Interface object pairing above reads a local rather than a class slot it
       has given away. */
    JS_SetClassProto(ctx, g_response_class, proto);

    /* §5.5's serializer, and Web IDL §3.8's property reference for the object built above — both over THIS realm's
       global, read before any of its scripts run. */
    {
        JSValue global = JS_GetGlobalObject(ctx);
        JSValue json = JS_GetPropertyStr(ctx, global, "JSON");
        JSValue stringify = JS_GetPropertyStr(ctx, json, "stringify");
        CHECK(JS_IsFunction(ctx, stringify),
              "%JSON.stringify% is not a function — Response.json has no serializer to run");
        realm_value_set(ctx, g_json_stringify_slot, stringify);
        JS_FreeValue(ctx, json);
        idl_define_global_property_reference(ctx, global, "Response", ctor);
        JS_FreeValue(ctx, global);
    }
}

void response_free(JSContext *ctx)
{
    /* the prototypes and this realm's serializer are the REALMS' — released with their contexts */
    g_response_rt = NULL;
    g_ctor_stepid = g_json_stepid = g_redirect_stepid = -1;
}

/* THE REPLY THE TRUSTED HOST FETCHED — headers the page may not write, which is the §5.5 "immutable" guard and
   the reason that guard exists at all. */
JSValue response_new(JSContext *ctx, JSValueConst url_list, int status, const char *status_text,
                     const HeaderList *headers, const char *body, size_t body_len)
{
    ResponseData *d;
    JSValue obj;
    int64_t n = 0;
    int lr;

    /* THE FETCH'S OWN URL LIST, which is the whole of what `url` and `redirected` are. §4.1's main fetch says
       "If internalResponse's URL list is empty, then set it to a clone of request's URL list", so a reply a
       host answered carries at least the request's own URL; an empty one here is a host that reported no list
       at all, and `redirected` would then be false for every redirect there ever was.
       The read runs BEFORE the assert rather than inside it, because a DCHECK's condition is compiled out in
       release and this one allocates. */
    lr = JS_GetLength(ctx, url_list, &n);
    DCHECK(JS_IsArray(url_list) && lr == 0 && n >= 1,
           "a host delivered a reply whose URL list is missing or empty — §4.1 gives a fetched response at "
           "least a clone of the request's URL list, so `url` and `redirected` have nothing to be computed "
           "from and the host's reply record is the thing to fix");
    if (lr < 0)
        return JS_EXCEPTION;
    obj = response_alloc(ctx, &d);
    if (JS_IsException(obj))
        return obj;
    /* THE REPLY'S OWN status and headers, not this component's guesses. They were 200/"OK"/none, which is what
       a host that could only hand over bytes forced — and a page reading `r.status` or `r.headers.get(...)` off
       a real reply got an answer this file invented. The guard is §5.5's "immutable": a reply the page did not
       construct is not a reply the page may rewrite. */
    if (response_set(ctx, d, url_list, status, status_text ? status_text : "", RESPONSE_TYPE_BASIC,
                     body ? body : "", body_len, headers, HEADERS_GUARD_IMMUTABLE) < 0) {
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    return obj;
}
