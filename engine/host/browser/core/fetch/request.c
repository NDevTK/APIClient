/* THE REQUEST INTERFACE — WHATWG Fetch §5.4 "Request class".
 *
 * WHAT IT IS FOR HERE. `fetch(input, init)` takes a `RequestInfo`, which is a Request or a string, so the
 * interface is half of the fetch method's own argument. It is also where two of §5.1's guards become
 * observable at all: a page's own `new Headers()` refuses nothing, and only a Request's header list carries
 * the "request" guard that drops `Host`, `Cookie` and a method-override header smuggling CONNECT/TRACE/TRACK,
 * or the "request-no-cors" guard that keeps only the four no-CORS-safelisted names.
 *
 * THE CONSTRUCTOR IS A MACHINE because every one of its inputs is the page's: `input` may be a string whose
 * `toString` the page wrote, `init` is a dictionary of [[Get]]s, and `init.headers` is the whole fill.
 *
 * §5.4's `signal` IS DOM §3.2 Interface AbortSignal's DEPENDENT ABORT SIGNAL AND NOT A STORED REFERENCE, which is the
 * whole reason it is this interface's own state rather than a member read back off `init`. The constructor's steps
 * are "let signals be « signal » if signal is non-null; otherwise « »" and "set this's signal to the result of
 * creating a dependent abort signal from signals" — so EVERY Request has one, a Request built with no `init.signal`
 * has a fresh never-aborting signal rather than null, and one built with a signal has a NEW signal that aborts when
 * that one does. §5.4 states the invariant in the getter's own prose ("this's signal is always initialized in the
 * constructor and when cloning"), which is why the getter asserts it instead of admitting an absence. The dependency
 * is core/dom/abort.h's `abort_signal_dependent_new`, which is DOM §3.2 Interface AbortSignal's one algorithm — an
 * imitation that added an abort ALGORITHM to the source would abort one turn late and the page can see the
 * difference.
 *
 * WHAT IS HONESTLY ABSENT: `body` as a ReadableStream, and `formData()`/`blob()`. Each is absent rather than
 * answered wrongly. */
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "quickjs-step.h"
#include "core/dom/abort.h"
#include "core/fetch/fetch.h"
#include "core/file/blob.h"
#include "core/fetch/request.h"
#include "core/fetch/headers.h"
#include "core/fetch/body.h"
#include "core/idl_args.h"
#include "core/realm.h"
#include "core/streams/readable_stream.h"
#include "core/url/url.h"

/* §5.4's request, as the fields the interface reports. The enumerated members are stored as their spec strings
   because that is what the attributes return and what `init` supplies — there is no computation on them yet,
   and inventing an enum would be a second spelling to keep in step with the first. */
typedef struct {
    char     *url;             /* the serialized parsed URL */
    char     *method;          /* normalized: uppercase for the six the spec names */
    char     *mode;            /* "cors" | "no-cors" | "same-origin" | "navigate" */
    char     *credentials;     /* "omit" | "same-origin" | "include" */
    char     *cache;
    char     *redirect;
    char     *referrer;
    char     *referrer_policy;
    char     *integrity;
    char     *destination;
    int       keepalive;
    BodyState body;
    JSValue   headers;         /* [SameObject] */
    /* §5.4: a Request built from a `blob:` URL CAPTURES its blob URL entry, so revoking the URL afterwards
       does not stop that request — the entry is the Request's, not the store's. Without it, the ordinary
       `new Request(u); URL.revokeObjectURL(u); fetch(req)` sequence a page uses to clean up eagerly failed. */
    JSValue blob_entry;
    /* §5.4's signal — §3.2's dependent abort signal, built at construction and at clone. It is OWNED, so it is
       freed by the finalizer, MARKED by the gc_mark (the collector cannot see through a class opaque) and
       DUP'd by every clone: a field added to this struct is an obligation at all three sites, and the three
       are read together for exactly that reason. */
    JSValue signal;
} RequestData;

static JSClassID g_request_class;
static JSRuntime *g_request_rt;
static int       g_request_ctor_stepid = -1;
static int       g_request_body_handle = -1;

static void request_finalizer(JSRuntime *rt, JSValue val)
{
    RequestData *d = JS_GetOpaque(val, g_request_class);
    if (!d) return;
    JS_FreeValueRT(rt, d->headers);
    JS_FreeValueRT(rt, d->blob_entry);
    JS_FreeValueRT(rt, d->signal);
    js_free_rt(rt, d->url); js_free_rt(rt, d->method); js_free_rt(rt, d->mode);
    js_free_rt(rt, d->credentials); js_free_rt(rt, d->cache); js_free_rt(rt, d->redirect);
    js_free_rt(rt, d->referrer); js_free_rt(rt, d->referrer_policy); js_free_rt(rt, d->integrity);
    js_free_rt(rt, d->destination); body_state_free(rt, &d->body);
    js_free_rt(rt, d);
}

/* The Headers a Request holds is a JSValue in the class opaque, which the collector cannot see through. */
static void request_gc_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    RequestData *d = JS_GetOpaque(val, g_request_class);
    if (d) {
        JS_MarkValue(rt, d->headers, mark_func);
        JS_MarkValue(rt, d->blob_entry, mark_func);
        JS_MarkValue(rt, d->signal, mark_func);
    }
    if (d) body_state_mark(rt, &d->body, mark_func);
}

static RequestData *request_of(JSValueConst v) { return JS_GetOpaque(v, g_request_class); }

/* §5.4's `formData()` asks the including interface for its Content-Type, because only it knows where its
   headers live. NULL when there is none, which is a body with no form encoding and therefore a TypeError. */
static char *request_body_mime(JSContext *ctx, JSValueConst v)
{
    RequestData *d = JS_GetOpaque(v, g_request_class);
    (void)ctx;
    return d ? header_list_get(headers_list_of(d->headers), "content-type") : NULL;
}

static BodyState *request_body_of(JSValueConst v)
{
    RequestData *d = JS_GetOpaque(v, g_request_class);
    return d ? &d->body : NULL;
}

/* THE BRAND — see request.h. The same `JS_GetOpaque` against this file's class id that every accessor here
   already uses to decide whether it is holding one of ours; it is public because a UNION resolves on exactly
   this question and the answer may not be duck-typed at the call site. */
bool request_is(JSValueConst v)
{
    return g_request_class && JS_GetOpaque(v, g_request_class) != NULL;
}

/* §5.4's captured blob URL entry, or JS_UNDEFINED — what a fetch of this Request answers from. Borrowed. */
JSValueConst request_blob_entry(JSValueConst v)
{
    RequestData *d = g_request_class ? JS_GetOpaque(v, g_request_class) : NULL;
    return d ? d->blob_entry : JS_UNDEFINED;
}

/* §2.2.1's "normalize a method": UPPERCASE for the six HTTP names, and byte-for-byte for anything else — `patch`
   stays lowercase while `post` becomes `POST`, which is the difference a server sees. */
static char *method_normalize(JSContext *ctx, const char *m)
{
    static const char *const UP[] = { "DELETE", "GET", "HEAD", "OPTIONS", "POST", "PUT" };
    size_t i;
    for (i = 0; i < sizeof(UP) / sizeof(UP[0]); i++)
        if (!strcasecmp(m, UP[i])) return js_strdup(ctx, UP[i]);
    return js_strdup(ctx, m);
}

/* §2.2.1's "forbidden method": the three a page may never send, however it spells them. */
static int method_is_forbidden(const char *m)
{
    return !strcasecmp(m, "CONNECT") || !strcasecmp(m, "TRACE") || !strcasecmp(m, "TRACK");
}

/* §2.2.1's "method": an RFC 7230 token, which is what makes `new Request(u, {method: "G ET"})` a TypeError. */
static int method_is_token(const char *s)
{
    const unsigned char *p = (const unsigned char *)s;
    if (!*p) return 0;
    for (; *p; p++) {
        unsigned char c = *p;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) continue;
        if (c && strchr("!#$%&'*+-.^_`|~", (char)c)) continue;
        return 0;
    }
    return 1;
}

bool request_method_is_token(const char *m) { return method_is_token(m) != 0; }

/* §5.4 STEP 25 AS ONE OPERATION, because two call sites perform it: this constructor, and `fetch(input, init)`,
   which runs §5.4 inline over an init it never turns into a Request. fetch() had none of it — no token test, no
   forbidden-method refusal and no normalization — so `{method:"connect"}` went out and `{method:"post"}` was
   reported on the endpoint surface as a method distinct from POST. Returns the normalized method (the caller
   frees it with js_free) or NULL with a TypeError live. */
char *request_method_check(JSContext *ctx, const char *m)
{
    if (!method_is_token(m) || method_is_forbidden(m)) {
        JS_ThrowTypeError(ctx, "the Request method is not a usable method");
        return NULL;
    }
    return method_normalize(ctx, m);
}

enum { REQ_METHOD = 0, REQ_URL, REQ_HEADERS, REQ_DESTINATION, REQ_REFERRER, REQ_REFERRER_POLICY,
       REQ_MODE, REQ_CREDENTIALS, REQ_CACHE, REQ_REDIRECT, REQ_INTEGRITY, REQ_KEEPALIVE,
       REQ_IS_RELOAD_NAV, REQ_IS_HISTORY_NAV, REQ_DUPLEX, REQ_SIGNAL };

static JSValue js_request_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    RequestData *d = request_of(this_val);
    if (!d)
        return JS_ThrowTypeError(ctx, "not a Request");
    switch (magic) {
    case REQ_METHOD:          return JS_NewString(ctx, d->method);
    case REQ_URL:             return JS_NewString(ctx, d->url);
    case REQ_HEADERS:         return JS_DupValue(ctx, d->headers);   /* [SameObject] */
    case REQ_DESTINATION:     return JS_NewString(ctx, d->destination);
    case REQ_REFERRER:        return JS_NewString(ctx, d->referrer);
    case REQ_REFERRER_POLICY: return JS_NewString(ctx, d->referrer_policy);
    case REQ_MODE:            return JS_NewString(ctx, d->mode);
    case REQ_CREDENTIALS:     return JS_NewString(ctx, d->credentials);
    case REQ_CACHE:           return JS_NewString(ctx, d->cache);
    case REQ_REDIRECT:        return JS_NewString(ctx, d->redirect);
    case REQ_INTEGRITY:       return JS_NewString(ctx, d->integrity);
    case REQ_KEEPALIVE:       return JS_NewBool(ctx, d->keepalive != 0);
    case REQ_SIGNAL:
        /* §5.4: "this's signal is always initialized in the constructor and when cloning" — the member is a
           non-nullable `AbortSignal`, so an absence here is an engine defect and never a value to report. */
        DCHECK(abort_signal_is(ctx, d->signal),
               "a Request answered `signal` with something that is not an AbortSignal — §5.4 builds one in the "
               "constructor and one in clone(), so a Request that reached a page without one was minted by a "
               "third path that owes itself the dependent signal both of those create");
        return JS_DupValue(ctx, d->signal);
    /* §5.4: both are false for a request a script constructed — only a navigation sets them, and there is no
       navigation here to set them from. This is a computed answer, not a placeholder. */
    case REQ_IS_RELOAD_NAV:
    case REQ_IS_HISTORY_NAV:  return JS_FALSE;
    default:
        DCHECK(magic == REQ_DUPLEX, "a Request accessor was declared with a magic this component does not answer");
        return JS_NewString(ctx, "half");
    }
}

/* §5.4 clone(). Like Response's, the "unusable" check is SYNCHRONOUS — a page that guards with try/catch sees
   the throw where it wrote the catch — and the clone gets its OWN body-used latch, because the point of
   cloning is two independent reads. The headers are a NEW Headers over the same list with the same guard:
   [SameObject] is per request, so `r.clone().headers === r.headers` is false. */
static JSValue js_request_clone(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    RequestData *d = request_of(this_val), *c;
    JSValue obj;

    (void)argc; (void)argv;
    if (!d)
        return JS_ThrowTypeError(ctx, "not a Request");
    if (d->body.used)
        return JS_ThrowTypeError(ctx, "cannot clone a Request whose body has been read");
    {
        JSValue rproto = JS_GetClassProto(ctx, g_request_class);
        DCHECK(!JS_IsNull(rproto), "a Request was minted in a realm that never ran its install");
        obj = JS_NewObjectProtoClass(ctx, rproto, g_request_class);
        JS_FreeValue(ctx, rproto);
    }
    if (JS_IsException(obj))
        return obj;
    c = js_mallocz(ctx, sizeof(*c));
    if (!c) { JS_FreeValue(ctx, obj); return JS_EXCEPTION; }
    c->headers = JS_UNDEFINED;
    c->signal  = JS_UNDEFINED;   /* placed before the first step that can fail — see the constructor's */
    JS_SetOpaque(obj, c);
    /* §5.4 clone copies the request, and its blob URL ENTRY is part of what it is — a clone of a request built
       from a since-revoked URL fetches exactly as the original does. */
    c->blob_entry      = JS_DupValue(ctx, d->blob_entry);
    c->url             = js_strdup(ctx, d->url);
    c->method          = js_strdup(ctx, d->method);
    c->mode            = js_strdup(ctx, d->mode);
    c->credentials     = js_strdup(ctx, d->credentials);
    c->cache           = js_strdup(ctx, d->cache);
    c->redirect        = js_strdup(ctx, d->redirect);
    c->referrer        = js_strdup(ctx, d->referrer);
    c->referrer_policy = js_strdup(ctx, d->referrer_policy);
    c->integrity       = js_strdup(ctx, d->integrity);
    c->destination     = js_strdup(ctx, d->destination);
    c->keepalive       = d->keepalive;
    if (!c->url || !c->method || !c->mode || !c->credentials || !c->cache || !c->redirect || !c->referrer ||
        !c->referrer_policy || !c->integrity || !c->destination ||
        body_state_set(ctx, &c->body, d->body.has ? d->body.bytes : NULL, d->body.len) < 0) {
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    c->headers = headers_new(ctx, headers_list_of(d->headers), headers_guard_of(d->headers));
    if (JS_IsException(c->headers)) { c->headers = JS_UNDEFINED; JS_FreeValue(ctx, obj); return JS_EXCEPTION; }
    /* §5.4 clone(): "assert: this's signal is non-null", then "let clonedSignal be the result of creating a
       dependent abort signal from « this's signal »". The clone does NOT share the signal object — aborting
       the original's signal aborts the clone's through the dependency, and the two are still `!==`. */
    DCHECK(abort_signal_is(ctx, d->signal),
           "a Request being cloned carried no AbortSignal — §5.4's clone steps assert this's signal is "
           "non-null, and every path that mints a Request builds one");
    {
        JSValueConst source[1];
        source[0] = d->signal;
        c->signal = abort_signal_dependent_new(ctx, source, 1);
    }
    if (JS_IsException(c->signal)) { c->signal = JS_UNDEFINED; JS_FreeValue(ctx, obj); return JS_EXCEPTION; }
    return obj;
}

/* ---- the constructor ------------------------------------------------------------------------------------- */

/* WHERE THIS MACHINE RESTS, AS §5.4 NUMBERS IT. The constructor's forty-two steps run the page's code in
   exactly two places — the header fill at step 33 and nothing else before it, and the body extraction at
   step 37 — so the stages are the three spans those two points cut it into, and each label says its span. The
   `stage` byte this state carried said none of that: a flow parked here could be described only as "stage 1 of
   something", which is neither a resume point a later build can resolve nor a thing a park can report. */
#define REQ_CTOR_STAGES(X) \
    X(REQ_CTOR_RECORD = IDL_STEP_FIRST, \
      "Fetch §5.4 new Request(input, init) steps 5-30 (the request record: step 12's carry-forward of a " \
      "Request input's URL, method, header list, mode, credentials mode, cache mode, redirect mode, " \
      "integrity and keepalive, then every init member that overrides one)") \
    X(REQ_CTOR_HEADERS, \
      "Fetch §5.4 new Request(input, init) steps 31-33 (step 32's CORS-safelisted-method test under a " \
      "\"no-cors\" mode, then this's headers under the guard that step chose)") \
    X(REQ_CTOR_BODY, \
      "Fetch §5.4 new Request(input, init) steps 34-41 (a body on GET or HEAD is a TypeError whether it is " \
      "the init's or the input Request's; extract init[\"body\"], or proxy the input Request's)")
enum { REQ_CTOR_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const REQ_CTOR_STEPS[] = { REQ_CTOR_STAGES(JS_STEP_STAGE_LABEL) NULL };

typedef struct {
    HeadersFill fill;
    HeaderList  list;
    JSValue     result;
} JSRequestCtorState;

static void js_request_ctor_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSRequestCtorState *s = st;
    headers_fill_visit(ctx, &s->fill, v);
    v->val(ctx, &s->result);
}

/* The header list alone — see js_headers_ctor_release. The rest is js_request_ctor_visit's declaration. */
static void js_request_ctor_release(JSContext *ctx, void *st)
{
    (void)ctx;
    header_list_free(&((JSRequestCtorState *)st)->list);
}

/* `dictionary RequestInit`, IN ONE PLACE, because it has TWO readers and they must not drift: the declaration
   below hands it to the argument machine, and req_init_is_empty walks the same names to answer §5.4 step 13's
   "If init is not empty". A second hand-written list of member names is how that question starts answering for
   a dictionary the declaration no longer has. Web IDL reads members in LEXICOGRAPHIC order, which is not the
   order §5.4 uses them in.
   `AbortSignal? signal` CARRIES NO DEFAULT, which is Fetch's own IDL and is load-bearing here rather than
   pedantic: a defaulted member is PRESENT on every converted dictionary, so `signal` with an IDL default made
   `new Request(r, {})` a NON-EMPTY init and step 13 would have reset the referrer of every construction there
   has ever been. The body reads it with abort_signal_is, which answers false for an absent member exactly as
   it does for `null`, so nothing needed the default. */
static const IdlDictMember REQUEST_INIT[] = {
    { "body",           IDL_BODYINIT_NULLABLE,  false },
    { "cache",          IDL_DOMSTRING,          false },
    { "credentials",    IDL_DOMSTRING,          false },
    { "headers",        IDL_ANY,                false },   /* HeadersInit: the union the fill converts */
    { "integrity",      IDL_DOMSTRING,          false },
    /* `boolean keepalive` declares NO default in Fetch's IDL, so IDL_BOOLEAN_NO_DEFAULT and not IDL_BOOLEAN:
       step 24 sets the request's keepalive only "if init["keepalive"] exists", and a member converted with
       ToBoolean(undefined) answers `false` for an absent one — which would overwrite the keepalive a Request
       input contributed at step 12 with a value the page never wrote. */
    { "keepalive",      IDL_BOOLEAN_NO_DEFAULT, false },
    { "method",         IDL_BYTESTRING,         false },
    { "mode",           IDL_DOMSTRING,          false },
    { "redirect",       IDL_DOMSTRING,          false },
    { "referrer",       IDL_USVSTRING,          false },
    { "referrerPolicy", IDL_DOMSTRING,          false },
    /* `AbortSignal? signal` — a DECLARED interface type, so `new Request(u, {signal: 5})` is Web IDL's
       own TypeError thrown before this constructor's first step rather than a check the body makes. */
    { "signal",         IDL_INTERFACE_NULLABLE, false },
};
#define REQUEST_INIT_N ((int)(sizeof(REQUEST_INIT) / sizeof(REQUEST_INIT[0])))

/* Web IDL §3.2.17 Dictionary types: a member the page did not supply is NOT ON the converted dictionary, and
   `undefined` is how that reads — which is the same test §5.4 spells "init[member] exists". Every member of
   REQUEST_INIT declares no default (see above), so absence and `undefined` coincide for all of them. */
static bool init_has(JSContext *ctx, JSValueConst init, const char *name)
{
    JSValue v = idl_dict_get(ctx, init, name);
    bool present = !JS_IsUndefined(v);

    JS_FreeValue(ctx, v);
    return present;
}

/* §5.4 step 13's "If init is not empty" — Infra's emptiness of the converted DICTIONARY, so it is a question
   about which members are PRESENT and never about which properties the page's object literal had. */
static bool req_init_is_empty(JSContext *ctx, JSValueConst init)
{
    int i;

    for (i = 0; i < REQUEST_INIT_N; i++)
        if (init_has(ctx, init, REQUEST_INIT[i].name))
            return false;
    return true;
}

/* A dictionary member as a plain string, or `dflt` when the page did not supply it. The declaration has
   already converted each to a real string, so this reads an engine-built object and runs nothing.
   `dflt` IS THE VALUE STEP 12 CARRIED FORWARD, never a constant the call site picked: §5.4 sets a request's
   mode / credentials mode / cache mode / redirect mode / integrity metadata from the INIT MEMBER ONLY WHERE IT
   EXISTS, and from the input request otherwise. A `?:` PAST A FAILED CONVERSION IS GONE with it — the read
   could only fail on OOM, and answering that with the default turned "this engine could not allocate" into
   "the page asked for same-origin credentials", which is a plausible datum where a crash belonged. */
static char *init_str(JSContext *ctx, JSValueConst init, const char *name, const char *dflt)
{
    JSValue v = idl_dict_get(ctx, init, name);
    char *r;
    if (JS_IsUndefined(v)) { JS_FreeValue(ctx, v); return js_strdup(ctx, dflt); }
    {
        const char *c = JS_ToCString(ctx, v);
        CHECK(c != NULL, "OOM reading a RequestInit member the declaration has already converted to a string");
        r = js_strdup(ctx, c);
        JS_FreeCString(ctx, c);
    }
    JS_FreeValue(ctx, v);
    return r;
}

static int js_request_ctor_step(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                                JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    JSRequestCtorState *s = st;
    JSValueConst input = argc > 0 ? argv[0] : JS_UNDEFINED;
    JSValueConst init = argc > 1 ? argv[1] : JS_UNDEFINED;
    RequestData *d;
    int r;

    if (hdr->stage == REQ_CTOR_RECORD) {
        JSValue obj;
        /* §5.4 step 6: "Set request to input's request". `from` IS that request — every field of it, not the
           URL alone — and steps 12 and 13 below are what carry it forward. */
        RequestData *from = request_of(input);
        const char *from_request = from ? from->url : NULL;
        bool init_not_empty = !req_init_is_empty(ctx, init);

        if (JS_IsUndefined(hdr->this_val)) {
            JS_FreeValue(ctx, cb_result);
            JS_ThrowTypeError(ctx, "constructor Request requires 'new'");
            return -1;
        }
        {
            JSValue rproto = JS_GetClassProto(ctx, g_request_class);
            DCHECK(!JS_IsNull(rproto), "a Request was minted in a realm that never ran its install");
            obj = JS_NewObjectProtoClass(ctx, rproto, g_request_class);
            JS_FreeValue(ctx, rproto);
        }
        if (JS_IsException(obj)) { JS_FreeValue(ctx, cb_result); return -1; }
        d = js_mallocz(ctx, sizeof(*d));
        if (!d) { JS_FreeValue(ctx, obj); JS_FreeValue(ctx, cb_result); return -1; }
        d->headers = JS_UNDEFINED;
        d->blob_entry = JS_UNDEFINED;
        /* EVERY OWNED FIELD IS PLACED BEFORE THE FIRST STEP THAT CAN THROW — the failure path frees exactly
           what the record holds, so a field handed over late is one the teardown reads uninitialised. The
           signal is BUILT at the end of this stage, where §5.4 builds it. */
        d->signal = JS_UNDEFINED;
        JS_SetOpaque(obj, d);
        s->result = obj;

        /* §5.4 step 5: a STRING input is parsed against the base URL, and a failure is a TypeError. A REQUEST
           input contributes its URL already parsed. THE TWO ARMS DIFFER HERE AND AT EVERY MEMBER BELOW, which
           is the correction this comment used to be the whole of: it said "which is why the two arms differ
           only here", and step 12 lists thirteen properties a Request input contributes — method, header
           list, referrer, referrer policy, mode, CREDENTIALS MODE, cache mode, redirect mode, integrity
           metadata and keepalive among them. Taking only the URL meant `new Request(r)` — the request-wrapping
           idiom every fetch interceptor is written in — answered POST as GET, `include` as `same-origin` and
           `no-cors` as `cors`, out of the DEFAULTS below rather than out of anything the page wrote. Each was
           a REAL value belonging to some request, which is what made it invisible: a wrong answer the engine
           then modelled, and every @H example value and @S verdict downstream of one is derived from a request
           the page never made. */
        if (from_request) {
            d->url = js_strdup(ctx, from_request);
        } else {
            UrlRecord rec;
            size_t n = 0;
            const char *in = JS_ToCStringLen(ctx, &n, input);
            bool ok;
            JS_FreeValue(ctx, cb_result);
            cb_result = JS_UNDEFINED;
            if (!in) return -1;
            /* AGAINST THE API BASE URL, through Fetch's one parse. `new Request("/api/users")` is how a
               bundle names its own endpoints, and with a NULL base every one of them was a TypeError. */
            ok = fetch_parse_url(ctx, &rec, in, n);
            JS_FreeCString(ctx, in);
            if (!ok) {
                url_record_free(&rec);
                JS_ThrowTypeError(ctx, "the Request input is not a valid URL");
                return -1;
            }
            /* §5.4 step 2.2: a URL with credentials is a TypeError — a page may not put a password on the
               wire by writing it into a fetch. */
            if ((rec.username && *rec.username) || (rec.password && *rec.password)) {
                url_record_free(&rec);
                JS_ThrowTypeError(ctx, "the Request input URL includes credentials");
                return -1;
            }
            {
                /* url_serialize returns a plain malloc'd string, and everything this struct holds is freed
                   with the ENGINE's allocator — mixing the two is a heap corruption that only shows up at
                   teardown, which is exactly where it showed up. Copy in, free out. */
                char *ser = url_serialize(&rec, false);
                d->url = js_strdup(ctx, ser);
                free(ser);
                /* §5.4: RESOLVE the blob URL now and hold what it named. A page that revokes eagerly —
                   `const r = new Request(u); URL.revokeObjectURL(u); fetch(r)` — still fetches, because the
                   entry belongs to the request from this moment. The FRAGMENT is not part of the entry's
                   identity, which is why the lookup key excludes it. */
                if (rec.scheme && !strcmp(rec.scheme, "blob")) {
                    char *key = url_serialize(&rec, true);
                    d->blob_entry = blob_url_lookup(ctx, key, strlen(key));
                    free(key);
                }
            }
            url_record_free(&rec);
        }

        /* THE INIT MEMBERS, IN §5.4'S OWN ORDER — steps 14 through 25. They were applied in the order this
           file grew in (method first, then mode, then the rest), and the order decides WHICH TypeError a page
           sees when two members are bad at once: §5.4 refuses a "navigate" mode at step 17, before it looks at
           the method at step 25. None of this runs the page's code — the dictionary conversion did that
           already — so the whole span is one stage, and the span is what the label names.
           EVERY DEFAULT BELOW IS NOW STEP 12'S CARRY-FORWARD, and the bare constant is what a STRING input
           starts from. §5.4 spells each of these "If init[member] exists, then set request's <field> to it",
           over a request that already holds the input's value — so a constant in the `dflt` position is a
           statement that the page asked for it, and for a Request input that statement is false. */
        /* §5.4 step 13: "If init is not empty" resets the referrer to "client" and the referrer policy to the
           empty string BEFORE steps 14-15 read the members — so a Request input's referrer survives only a
           construction that supplies no member at all. This is the one place step 13 is observable here: its
           other clauses are the reload/history-navigation flags and the origin, which this engine's Request
           record does not hold, and the "navigate" mode it rewrites is asserted unreachable below. */
        d->referrer        = init_str(ctx, init, "referrer",                          /* steps 13-14 */
                                      (from && !init_not_empty) ? from->referrer : "about:client");
        d->referrer_policy = init_str(ctx, init, "referrerPolicy",                    /* steps 13, 15 */
                                      (from && !init_not_empty) ? from->referrer_policy : "");
        /* §5.4 steps 16-18: "navigate" is not a mode a page may ask for. Step 16 is "Let mode be init["mode"]
           if it exists, and FALLBACKMODE otherwise" — and fallbackMode is set to "cors" at step 5.5, the
           STRING arm, and left null by the Request arm; step 18's "If mode is non-null" is what then leaves a
           Request input's own mode standing. So "cors" here is the string input's fallback and never a
           Request's. */
        DCHECK(!from || strcmp(from->mode, "navigate") != 0,
               "a Request used as `input` carried mode \"navigate\" — step 17 throws on it and no other path "
               "in this engine mints a Request, so step 13's \"if request's mode is navigate, set it to "
               "same-origin\" is unreachable rather than unimplemented");
        d->mode = init_str(ctx, init, "mode", from ? from->mode : "cors");
        if (!strcmp(d->mode, "navigate")) {
            JS_ThrowTypeError(ctx, "a Request cannot be constructed with mode \"navigate\"");
            return -1;
        }
        d->credentials     = init_str(ctx, init, "credentials",                       /* step 19 */
                                      from ? from->credentials : "same-origin");
        d->cache           = init_str(ctx, init, "cache", from ? from->cache : "default");   /* step 20 */
        /* §5.4 step 21: "only-if-cached" asks the cache to answer without going to the network, which only
           means anything for a same-origin request — so any other mode is a TypeError. It was missing, and
           `new Request(u, {cache:"only-if-cached"})` (mode "cors" by step 5.5's fallback) was accepted. */
        if (!strcmp(d->cache, "only-if-cached") && strcmp(d->mode, "same-origin")) {
            JS_ThrowTypeError(ctx, "a Request with cache \"only-if-cached\" must have mode \"same-origin\"");
            return -1;
        }
        d->redirect        = init_str(ctx, init, "redirect", from ? from->redirect : "follow");   /* step 22 */
        d->integrity       = init_str(ctx, init, "integrity", from ? from->integrity : "");       /* step 23 */
        /* §5.4 step 24: "If init["keepalive"] exists, then set request's keepalive to it" — so an ABSENT
           member leaves step 12's value standing, which is the input request's or `false` for a string. */
        d->keepalive = from ? from->keepalive : 0;                                    /* step 12 */
        if (init_has(ctx, init, "keepalive")) {                                       /* step 24 */
            JSValue kv = idl_dict_get(ctx, init, "keepalive");
            d->keepalive = JS_ToBool(ctx, kv);
            JS_FreeValue(ctx, kv);
        }
        /* §5.4 step 25's method: a token, not a forbidden method, then normalized. An absent member leaves
           step 12's — the input request's already-normalized method, or `GET` for a string input. */
        {
            JSValue mv = idl_dict_get(ctx, init, "method");
            if (JS_IsUndefined(mv)) {
                d->method = js_strdup(ctx, from ? from->method : "GET");
            } else {
                const char *mc = JS_ToCString(ctx, mv);
                if (!mc) { JS_FreeValue(ctx, mv); return -1; }
                d->method = request_method_check(ctx, mc);
                JS_FreeCString(ctx, mc);
                if (!d->method) { JS_FreeValue(ctx, mv); return -1; }
            }
            JS_FreeValue(ctx, mv);
        }
        d->destination     = js_strdup(ctx, "");   /* §5.4: a script-constructed request has no destination */
        CHECK(d->url && d->method && d->mode && d->credentials && d->cache && d->redirect && d->referrer &&
              d->referrer_policy && d->integrity && d->destination, "request: OOM building a Request");
        /* §5.4 steps 4 / 6.3 / 26 and then "let signals be « signal » if signal is non-null; otherwise « »"
           and "set this's signal to the result of creating a dependent abort signal from signals". The two
           sources are the Request `input` (a `new Request(other)` inherits other's signal) and init["signal"],
           and init WINS because step 26 runs after step 6.3. It is built HERE — after every step of this stage
           that can throw and before the header fill, which is where §5.4 puts it — so a construction that
           refuses has registered no dependency on a signal the page still holds. */
        {
            JSValueConst sources[1];
            int n = 0;
            JSValue given = idl_dict_get(ctx, init, "signal");

            if (abort_signal_is(ctx, given))
                sources[n++] = given;
            else if (from) {
                DCHECK(abort_signal_is(ctx, from->signal),
                       "a Request used as `input` carried no AbortSignal — §5.4 gives every Request one, so a "
                       "source without one was built by a path that skipped the dependent signal");
                sources[n++] = from->signal;
            }
            d->signal = abort_signal_dependent_new(ctx, sources, n);
            JS_FreeValue(ctx, given);
            if (JS_IsException(d->signal)) { d->signal = JS_UNDEFINED; return -1; }
        }
        /* §5.4 step 12's HEADER LIST: "A copy of request's header list" — the input Request's, which this
           constructor took none of. Step 33 then re-appends every entry UNDER THE NEW REQUEST'S GUARD when
           init is not empty, which is what drops a `Range` that a `no-cors` mode no longer admits; the guard
           is computed here because it is a function of the mode this stage just settled. Running the guarded
           append unconditionally is the spec's own answer in every case this engine can reach: init being
           EMPTY is exactly the case in which the mode — and therefore the guard — is the input's own, and a
           list §5.1 built under a guard re-appends through that same guard unchanged.
           When init["headers"] EXISTS the copy is discarded rather than seeded: step 33's own sub-steps set
           `headers` to the init member and then empty this's header list, leaving nothing of the copy. */
        if (from && !init_has(ctx, init, "headers")) {
            const HeaderList *src = headers_list_of(from->headers);
            HeadersGuard guard = !strcmp(d->mode, "no-cors") ? HEADERS_GUARD_REQUEST_NO_CORS
                                                             : HEADERS_GUARD_REQUEST;
            int i;

            DCHECK(src != NULL, "a Request used as `input` carried no Headers object — §5.4 gives every "
                                "Request one at step 31, so a source without one was built by a path that "
                                "skipped it");
            for (i = 0; i < src->n; i++)
                if (header_list_append_guarded(ctx, &s->list, guard, src->e[i].name, src->e[i].value) < 0)
                    return -1;
        }
        headers_fill_init(&s->fill);
        hdr->stage = REQ_CTOR_HEADERS;
    }

    d = request_of(s->result);
    DCHECK(d != NULL, "the Request the constructor allocated stopped being one mid-construction");

    if (hdr->stage == REQ_CTOR_HEADERS) {
        /* §5.4's headers: guard "request", or "request-no-cors" when the mode says so — which is the ONLY way
           either guard becomes observable, since a page's own Headers has guard "none". */
        HeadersGuard guard = !strcmp(d->mode, "no-cors") ? HEADERS_GUARD_REQUEST_NO_CORS
                                                         : HEADERS_GUARD_REQUEST;
        JSValue hv;

        /* §5.4 step 32.1: under a "no-cors" mode the method must be a CORS-SAFELISTED METHOD — §2.2.1 Methods'
           `GET`, `HEAD` or `POST` — and anything else is a TypeError. It was missing, so
           `new Request(u, {mode:"no-cors", method:"PUT"})` built a request no browser will make, and the
           mode carried forward at step 12 gives that shape a second way in. The method here is normalized, so
           the comparison is against the uppercase spellings and nothing else. */
        if (guard == HEADERS_GUARD_REQUEST_NO_CORS && strcmp(d->method, "GET") &&
            strcmp(d->method, "HEAD") && strcmp(d->method, "POST")) {
            JS_FreeValue(ctx, cb_result);
            JS_ThrowTypeError(ctx, "a Request with mode \"no-cors\" must use a CORS-safelisted method");
            return -1;
        }
        hv = idl_dict_get(ctx, init, "headers");
        r = headers_fill_run(ctx, hdr, &s->fill, hv, &s->list, guard, cb_result, out_cb, out_argc);
        JS_FreeValue(ctx, hv);
        if (r > 0) return r;
        if (r < 0) return -1;
        cb_result = JS_UNDEFINED;
        JS_FreeValue(ctx, d->headers);
        d->headers = headers_new(ctx, &s->list, guard);
        if (JS_IsException(d->headers)) return -1;
        hdr->stage = REQ_CTOR_BODY;
    }

    DCHECK(hdr->stage == REQ_CTOR_BODY,
           "the Request constructor was re-entered at a stage §5.4 does not have");
    /* §5.4 steps 34-41. A body on a GET or a HEAD is a TypeError. The extraction itself is §5.2's, which
       body.c owns — both including interfaces run the same six-armed union, and the Content-Type it produces
       is set only where the header list has none.
       AND `inputBody` IS THE HALF THAT WAS ABSENT: step 34 is "Let inputBody be input's request's body if
       input is a Request object", and every step after it reads that name. Without it a `new Request(post)`
       came out with the method (now) and NO BODY, which is a request the server answers differently and the
       page never wrote — and step 35's TypeError, which is about the INHERITED body as much as the init's,
       could not fire at all. */
    JS_FreeValue(ctx, cb_result);
    {
        RequestData *from = request_of(input);
        bool input_body = from && from->body.has;
        JSValue bv = idl_dict_get(ctx, init, "body");
        bool init_body = !JS_IsUndefined(bv) && !JS_IsNull(bv);

        /* STEP 35: "If either init["body"] exists and is non-null OR INPUTBODY IS NON-NULL, and request's
           method is `GET` or `HEAD`, then throw a TypeError" — so `new Request(post, {method:"GET"})` is
           refused rather than silently issued bodiless. */
        if ((init_body || input_body) && (!strcmp(d->method, "GET") || !strcmp(d->method, "HEAD"))) {
            JS_FreeValue(ctx, bv);
            JS_ThrowTypeError(ctx, "a Request with a GET or HEAD method cannot have a body");
            return -1;
        }
        if (init_body) {                                                   /* step 37 */
            char *mime = NULL;
            /* §5.4 extracts "with keepalive set to request's keepalive", and that is the value step 24 read
               out of init a stage ago — not a constant. §5.2's ReadableStream arm begins "If keepalive is
               true, then throw a TypeError", so a hardcoded false made `new Request(u, {method: "POST",
               keepalive: true, body: stream})` build a request a browser refuses. */
            if (body_extract(ctx, &d->body, bv, d->keepalive != 0, &mime) < 0) {
                free(mime);
                JS_FreeValue(ctx, bv);
                return -1;
            }
            if (mime) {
                const HeaderList *hl = headers_list_of(d->headers);
                char *existing = header_list_get(hl, "content-type");
                if (!existing)
                    header_list_append((HeaderList *)hl, "content-type", mime);
                free(existing);
                free(mime);
            }
        }
        JS_FreeValue(ctx, bv);
        /* STEPS 38-41: "Let inputOrInitBody be initBody if it is non-null; otherwise inputBody" — so the
           input's body is this request's ONLY where the init supplied none, which is the whole of the
           `new Request(r, {...})` wrapper idiom. */
        if (!init_body && input_body) {
            bool locked = false;
            /* STEP 41.1: "If inputBody is unusable, then throw a TypeError" — §2.2.4 Bodies' unusable is
               DISTURBED **or** LOCKED, which is the pair Response.clone's own entry reports, and not the read
               latch alone: a body a page has taken a reader on cannot be proxied, and reporting only `used`
               would reach the proxy and fail inside it — the wrong error at the wrong place. */
            readable_stream_query(from->body.stream, NULL, &locked);
            if (from->body.used || readable_stream_disturbed(from->body.stream) || locked) {
                JS_ThrowTypeError(ctx, "a Request whose body is disturbed or locked cannot be used as the "
                                       "input of a new Request");
                return -1;
            }
            /* STEP 41.2: "set finalBody to the result of CREATING A PROXY FOR inputBody" — Streams §9.5
               Piping's create-a-proxy, which is NOT §2.2.4's clone: the input becomes locked and disturbed,
               so `new Request(r)` leaves `r.text()` throwing. body.c owns that distinction. */
            if (body_create_proxy(ctx, input, &from->body, &d->body) < 0)
                return -1;
        }
    }
    *presult = s->result;
    s->result = JS_UNDEFINED;
    return 0;
}

static const IdlStepDecl js_request_ctor_decl = {
    js_request_ctor_step, sizeof(JSRequestCtorState), js_request_ctor_visit, js_request_ctor_release,
    "Fetch §5.4 new Request(input, init)", REQ_CTOR_STEPS
};

static const JSCFunctionListEntry js_request_proto_funcs[] = {
    JS_CFUNC_DEF("clone", 0, js_request_clone),
    JS_CGETSET_MAGIC_DEF("method", js_request_get, NULL, REQ_METHOD),
    JS_CGETSET_MAGIC_DEF("url", js_request_get, NULL, REQ_URL),
    JS_CGETSET_MAGIC_DEF("headers", js_request_get, NULL, REQ_HEADERS),
    JS_CGETSET_MAGIC_DEF("destination", js_request_get, NULL, REQ_DESTINATION),
    JS_CGETSET_MAGIC_DEF("referrer", js_request_get, NULL, REQ_REFERRER),
    JS_CGETSET_MAGIC_DEF("referrerPolicy", js_request_get, NULL, REQ_REFERRER_POLICY),
    JS_CGETSET_MAGIC_DEF("mode", js_request_get, NULL, REQ_MODE),
    JS_CGETSET_MAGIC_DEF("credentials", js_request_get, NULL, REQ_CREDENTIALS),
    JS_CGETSET_MAGIC_DEF("cache", js_request_get, NULL, REQ_CACHE),
    JS_CGETSET_MAGIC_DEF("redirect", js_request_get, NULL, REQ_REDIRECT),
    JS_CGETSET_MAGIC_DEF("integrity", js_request_get, NULL, REQ_INTEGRITY),
    JS_CGETSET_MAGIC_DEF("keepalive", js_request_get, NULL, REQ_KEEPALIVE),
    JS_CGETSET_MAGIC_DEF("isReloadNavigation", js_request_get, NULL, REQ_IS_RELOAD_NAV),
    JS_CGETSET_MAGIC_DEF("isHistoryNavigation", js_request_get, NULL, REQ_IS_HISTORY_NAV),
    JS_CGETSET_MAGIC_DEF("duplex", js_request_get, NULL, REQ_DUPLEX),
    JS_CGETSET_MAGIC_DEF("signal", js_request_get, NULL, REQ_SIGNAL),
};

void request_init(JSContext *ctx)
{
    JSClassDef def = { "Request", .finalizer = request_finalizer, .gc_mark = request_gc_mark };
    JSRuntime *rt = JS_GetRuntime(ctx);
    /* `constructor(RequestInfo input, optional RequestInit init = {})`. REQUEST_INIT is at file scope because
       §5.4 step 13's emptiness test reads the same list; see its declaration. */
    static const IdlArgType CTOR_ARGS[2] = { IDL_ANY, IDL_DICT };   /* RequestInfo: the machine resolves it */

    DCHECK(g_request_rt == NULL || g_request_rt == rt,
           "Request was installed into a second runtime — its class id and step ids belong to the first, and "
           "one WASM instance is one document");
    if (g_request_rt == rt)
        return;
    g_request_rt = rt;
    JS_NewClassID(rt, &g_request_class);
    JS_NewClass(rt, g_request_class, &def);
    /* NO SOURCE: a Request's body is bytes the PAGE composed and handed to fetch(), never a byte sequence a
       server filled — see core/byte_reader.h for why NULL here is a statement and not a hole. */
    g_request_body_handle = body_declare(ctx, g_request_class, request_body_of, request_body_mime, NULL,
                                         "Request");
    g_request_ctor_stepid = idl_method_id_step(ctx, CTOR_ARGS, 2, REQUEST_INIT, REQUEST_INIT_N,
                                               &js_request_ctor_decl, 0);
    idl_optional_from(1);   /* §5.4: `optional RequestInit init = {}` */
    idl_iface_brand(abort_signal_class());   /* RequestInit's one interface-typed member */
    realm_declare_intrinsic(request_install_proto);
}

/* §5.4's INTERFACE PROTOTYPE OBJECT, FOR ONE REALM — `url` resolves against the READING realm's API base URL,
   so a shared one answers a child document's `new Request("api/x").url` with the parent's address. */
void request_install_proto(JSContext *ctx)
{
    JSValue proto, prev;

    DCHECK(g_request_class != 0, "a realm asked for Request.prototype before the class was declared");
    prev = JS_GetClassProto(ctx, g_request_class);
    DCHECK(JS_IsNull(prev), "request_install_proto ran twice in one realm");
    JS_FreeValue(ctx, prev);

    proto = JS_NewObject(ctx);
    CHECK(!JS_IsException(proto), "Request.prototype could not be allocated");
    idl_interface_tag(ctx, proto, "Request");
    JS_SetPropertyFunctionList(ctx, proto, js_request_proto_funcs,
                               (int)(sizeof(js_request_proto_funcs) / sizeof(js_request_proto_funcs[0])));
    body_install(ctx, proto, g_request_body_handle);
    JS_SetClassProto(ctx, g_request_class, proto);
}

void request_install(JSContext *ctx, JSValueConst global)
{
    JSValue ctor;
    DCHECK(g_request_ctor_stepid >= 0, "Request was installed before request_init declared its constructor");
    ctor = idl_step_constructor(ctx, "Request", g_request_ctor_stepid);
    CHECK(!JS_IsException(ctor), "the Request interface object could not be allocated");
    {
        JSValue proto = JS_GetClassProto(ctx, g_request_class);
        DCHECK(!JS_IsNull(proto), "Request was installed into a realm that never ran its proto build");
        JS_SetConstructor(ctx, ctor, proto);
        JS_FreeValue(ctx, proto);
    }
    JS_SetPropertyStr(ctx, (JSValue)global, "Request", ctor);
}

void request_free(JSContext *ctx)
{
    if (!g_request_rt)
        return;
    /* the prototypes are the REALMS' — released with their contexts */
    g_request_rt = NULL;
    g_request_ctor_stepid = -1;
}
