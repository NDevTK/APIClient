/* THE REQUEST INTERFACE — WHATWG Fetch §5.3.
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
 * WHAT IS HONESTLY ABSENT: `signal` (there is an AbortSignal component but no fetch to abort yet), `body` as a
 * ReadableStream, and `formData()`/`blob()`. Each is absent rather than answered wrongly — a `signal` that
 * returned null would be indistinguishable from one the page passed. */
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "quickjs-step.h"
#include "core/fetch/fetch.h"
#include "core/file/blob.h"
#include "core/fetch/request.h"
#include "core/fetch/headers.h"
#include "core/fetch/body.h"
#include "core/idl_args.h"
#include "core/url/url.h"

/* §5.3's request, as the fields the interface reports. The enumerated members are stored as their spec strings
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
    /* §5.3: a Request built from a `blob:` URL CAPTURES its blob URL entry, so revoking the URL afterwards
       does not stop that request — the entry is the Request's, not the store's. Without it, the ordinary
       `new Request(u); URL.revokeObjectURL(u); fetch(req)` sequence a page uses to clean up eagerly failed. */
    JSValue blob_entry;
} RequestData;

static JSClassID g_request_class;
static JSValue   g_request_proto = JS_UNDEFINED;
static JSRuntime *g_request_rt;
static int       g_request_ctor_stepid = -1;
static int       g_request_body_handle = -1;

static void request_finalizer(JSRuntime *rt, JSValue val)
{
    RequestData *d = JS_GetOpaque(val, g_request_class);
    if (!d) return;
    JS_FreeValueRT(rt, d->headers);
    JS_FreeValueRT(rt, d->blob_entry);
    js_free_rt(rt, d->url); js_free_rt(rt, d->method); js_free_rt(rt, d->mode);
    js_free_rt(rt, d->credentials); js_free_rt(rt, d->cache); js_free_rt(rt, d->redirect);
    js_free_rt(rt, d->referrer); js_free_rt(rt, d->referrer_policy); js_free_rt(rt, d->integrity);
    js_free_rt(rt, d->destination); js_free_rt(rt, d->body.bytes);
    js_free_rt(rt, d);
}

/* The Headers a Request holds is a JSValue in the class opaque, which the collector cannot see through. */
static void request_gc_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    RequestData *d = JS_GetOpaque(val, g_request_class);
    if (d) { JS_MarkValue(rt, d->headers, mark_func); JS_MarkValue(rt, d->blob_entry, mark_func); }
    if (d) body_state_mark(rt, &d->body, mark_func);
}

static RequestData *request_of(JSValueConst v) { return JS_GetOpaque(v, g_request_class); }

/* §5.2's `formData()` asks the including interface for its Content-Type, because only it knows where its
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

/* §5.3's captured blob URL entry, or JS_UNDEFINED — what a fetch of this Request answers from. Borrowed. */
JSValueConst request_blob_entry(JSValueConst v)
{
    RequestData *d = g_request_class ? JS_GetOpaque(v, g_request_class) : NULL;
    return d ? d->blob_entry : JS_UNDEFINED;
}

const char *request_url_of(JSValueConst v)
{
    RequestData *d = JS_GetOpaque(v, g_request_class);
    return d ? d->url : NULL;
}

/* §5.3 "normalize a method": UPPERCASE for the six HTTP names, and byte-for-byte for anything else — `patch`
   stays lowercase while `post` becomes `POST`, which is the difference a server sees. */
static char *method_normalize(JSContext *ctx, const char *m)
{
    static const char *const UP[] = { "DELETE", "GET", "HEAD", "OPTIONS", "POST", "PUT" };
    size_t i;
    for (i = 0; i < sizeof(UP) / sizeof(UP[0]); i++)
        if (!strcasecmp(m, UP[i])) return js_strdup(ctx, UP[i]);
    return js_strdup(ctx, m);
}

/* §5.3's "forbidden method": the three a page may never send, however it spells them. */
static int method_is_forbidden(const char *m)
{
    return !strcasecmp(m, "CONNECT") || !strcasecmp(m, "TRACE") || !strcasecmp(m, "TRACK");
}

/* §5.1's "method": an RFC 7230 token, which is what makes `new Request(u, {method: "G ET"})` a TypeError. */
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

enum { REQ_METHOD = 0, REQ_URL, REQ_HEADERS, REQ_DESTINATION, REQ_REFERRER, REQ_REFERRER_POLICY,
       REQ_MODE, REQ_CREDENTIALS, REQ_CACHE, REQ_REDIRECT, REQ_INTEGRITY, REQ_KEEPALIVE,
       REQ_IS_RELOAD_NAV, REQ_IS_HISTORY_NAV, REQ_DUPLEX };

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
    /* §5.3: both are false for a request a script constructed — only a navigation sets them, and there is no
       navigation here to set them from. This is a computed answer, not a placeholder. */
    case REQ_IS_RELOAD_NAV:
    case REQ_IS_HISTORY_NAV:  return JS_FALSE;
    default:
        DCHECK(magic == REQ_DUPLEX, "a Request accessor was declared with a magic this component does not answer");
        return JS_NewString(ctx, "half");
    }
}

/* §5.3 clone(). Like Response's, the "unusable" check is SYNCHRONOUS — a page that guards with try/catch sees
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
    obj = JS_NewObjectProtoClass(ctx, g_request_proto, g_request_class);
    if (JS_IsException(obj))
        return obj;
    c = js_mallocz(ctx, sizeof(*c));
    if (!c) { JS_FreeValue(ctx, obj); return JS_EXCEPTION; }
    c->headers = JS_UNDEFINED;
    JS_SetOpaque(obj, c);
    /* §5.3 clone copies the request, and its blob URL ENTRY is part of what it is — a clone of a request built
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
    return obj;
}

/* ---- the constructor ------------------------------------------------------------------------------------- */

typedef struct {
    uint8_t     stage;
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

static void js_request_ctor_release(JSContext *ctx, void *st)
{
    JSRequestCtorState *s = st;
    headers_fill_release(ctx, &s->fill);
    header_list_free(&s->list);
    JS_FreeValue(ctx, s->result);
    s->result = JS_UNDEFINED;
}

/* A dictionary member as a plain string, or `dflt` when the page did not supply it. The declaration has
   already converted each to a real string, so this reads an engine-built object and runs nothing. */
static char *init_str(JSContext *ctx, JSValueConst init, const char *name, const char *dflt)
{
    JSValue v = idl_dict_get(ctx, init, name);
    char *r;
    if (JS_IsUndefined(v)) { JS_FreeValue(ctx, v); return js_strdup(ctx, dflt); }
    {
        const char *c = JS_ToCString(ctx, v);
        r = js_strdup(ctx, c ? c : dflt);
        if (c) JS_FreeCString(ctx, c);
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

    if (s->stage == 0) {
        JSValue obj;
        const char *from_request = request_url_of(input);

        if (JS_IsUndefined(hdr->this_val)) {
            JS_FreeValue(ctx, cb_result);
            JS_ThrowTypeError(ctx, "constructor Request requires 'new'");
            return -1;
        }
        obj = JS_NewObjectProtoClass(ctx, g_request_proto, g_request_class);
        if (JS_IsException(obj)) { JS_FreeValue(ctx, cb_result); return -1; }
        d = js_mallocz(ctx, sizeof(*d));
        if (!d) { JS_FreeValue(ctx, obj); JS_FreeValue(ctx, cb_result); return -1; }
        d->headers = JS_UNDEFINED;
        d->blob_entry = JS_UNDEFINED;
        JS_SetOpaque(obj, d);
        s->result = obj;

        /* §5.3 step 2: a STRING input is parsed against the base URL, and a failure is a TypeError. A REQUEST
           input contributes its URL already parsed, which is why the two arms differ only here. */
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
            ok = fetch_parse_url(&rec, in, n);
            JS_FreeCString(ctx, in);
            if (!ok) {
                url_record_free(&rec);
                JS_ThrowTypeError(ctx, "the Request input is not a valid URL");
                return -1;
            }
            /* §5.3 step 2.2: a URL with credentials is a TypeError — a page may not put a password on the
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
                /* §5.3: RESOLVE the blob URL now and hold what it named. A page that revokes eagerly —
                   `const r = new Request(u); URL.revokeObjectURL(u); fetch(r)` — still fetches, because the
                   entry belongs to the request from this moment. The FRAGMENT is not part of the entry's
                   identity, which is why the lookup key excludes it. */
                if (rec.scheme && !strcmp(rec.scheme, "blob")) {
                    char *key = url_serialize(&rec, true);
                    d->blob_entry = JS_DupValue(ctx, blob_url_lookup(key, strlen(key)));
                    free(key);
                }
            }
            url_record_free(&rec);
        }

        /* §5.3's method: a token, not a forbidden method, then normalized. */
        {
            JSValue mv = idl_dict_get(ctx, init, "method");
            if (JS_IsUndefined(mv)) {
                d->method = js_strdup(ctx, "GET");
            } else {
                const char *mc = JS_ToCString(ctx, mv);
                if (!mc) { JS_FreeValue(ctx, mv); return -1; }
                if (!method_is_token(mc) || method_is_forbidden(mc)) {
                    JS_FreeCString(ctx, mc);
                    JS_FreeValue(ctx, mv);
                    JS_ThrowTypeError(ctx, "the Request method is not a usable method");
                    return -1;
                }
                d->method = method_normalize(ctx, mc);
                JS_FreeCString(ctx, mc);
            }
            JS_FreeValue(ctx, mv);
        }
        /* §5.3: "navigate" is not a mode a page may ask for. */
        d->mode = init_str(ctx, init, "mode", "cors");
        if (!strcmp(d->mode, "navigate")) {
            JS_ThrowTypeError(ctx, "a Request cannot be constructed with mode \"navigate\"");
            return -1;
        }
        d->credentials     = init_str(ctx, init, "credentials", "same-origin");
        d->cache           = init_str(ctx, init, "cache", "default");
        d->redirect        = init_str(ctx, init, "redirect", "follow");
        d->referrer        = init_str(ctx, init, "referrer", "about:client");
        d->referrer_policy = init_str(ctx, init, "referrerPolicy", "");
        d->integrity       = init_str(ctx, init, "integrity", "");
        d->destination     = js_strdup(ctx, "");   /* §5.3: a script-constructed request has no destination */
        {
            JSValue kv = idl_dict_get(ctx, init, "keepalive");
            d->keepalive = JS_ToBool(ctx, kv);
            JS_FreeValue(ctx, kv);
        }
        CHECK(d->url && d->method && d->mode && d->credentials && d->cache && d->redirect && d->referrer &&
              d->referrer_policy && d->integrity && d->destination, "request: OOM building a Request");
        headers_fill_init(&s->fill);
        s->stage = 1;
    }

    d = request_of(s->result);
    DCHECK(d != NULL, "the Request the constructor allocated stopped being one mid-construction");

    if (s->stage == 1) {
        /* §5.3's headers: guard "request", or "request-no-cors" when the mode says so — which is the ONLY way
           either guard becomes observable, since a page's own Headers has guard "none". */
        HeadersGuard guard = !strcmp(d->mode, "no-cors") ? HEADERS_GUARD_REQUEST_NO_CORS
                                                         : HEADERS_GUARD_REQUEST;
        JSValue hv = idl_dict_get(ctx, init, "headers");
        r = headers_fill_run(ctx, hdr, &s->fill, hv, &s->list, guard, cb_result, out_cb, out_argc);
        JS_FreeValue(ctx, hv);
        if (r > 0) return r;
        if (r < 0) return -1;
        cb_result = JS_UNDEFINED;
        JS_FreeValue(ctx, d->headers);
        d->headers = headers_new(ctx, &s->list, guard);
        if (JS_IsException(d->headers)) return -1;
        s->stage = 2;
    }

    DCHECK(s->stage == 2, "the Request constructor was re-entered at a stage it never parks in");
    JS_FreeValue(ctx, cb_result);
    /* §5.3: a body on a GET or a HEAD is a TypeError. The extraction itself is §5.1's, which body.c owns —
       both including interfaces run the same six-armed union, and the Content-Type it produces is set only
       where the header list has none. */
    {
        JSValue bv = idl_dict_get(ctx, init, "body");
        if (!JS_IsUndefined(bv) && !JS_IsNull(bv)) {
            char *mime = NULL;
            if (!strcmp(d->method, "GET") || !strcmp(d->method, "HEAD")) {
                JS_FreeValue(ctx, bv);
                JS_ThrowTypeError(ctx, "a Request with a GET or HEAD method cannot have a body");
                return -1;
            }
            if (body_extract(ctx, &d->body, bv, &mime) < 0) {
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
    }
    *presult = s->result;
    s->result = JS_UNDEFINED;
    return 0;
}

static const IdlStepDecl js_request_ctor_decl = {
    js_request_ctor_step, sizeof(JSRequestCtorState), js_request_ctor_visit, js_request_ctor_release
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
};

void request_init(JSContext *ctx)
{
    JSClassDef def = { "Request", .finalizer = request_finalizer, .gc_mark = request_gc_mark };
    JSRuntime *rt = JS_GetRuntime(ctx);
    /* `constructor(RequestInfo input, optional RequestInit init = {})`. The dictionary's members are listed in
       the order Web IDL reads them, which is lexicographic and not the order §5.3 uses them in. */
    static const IdlArgType CTOR_ARGS[2] = { IDL_ANY, IDL_DICT };   /* RequestInfo: the machine resolves it */
    static const IdlDictMember REQUEST_INIT[] = {
        { "body",           IDL_BODYINIT_NULLABLE, false },
        { "cache",          IDL_DOMSTRING,         false },
        { "credentials",    IDL_DOMSTRING,         false },
        { "headers",        IDL_ANY,               false },   /* HeadersInit: the union the fill converts */
        { "integrity",      IDL_DOMSTRING,         false },
        { "keepalive",      IDL_BOOLEAN,           false },
        { "method",         IDL_BYTESTRING,        false },
        { "mode",           IDL_DOMSTRING,         false },
        { "redirect",       IDL_DOMSTRING,         false },
        { "referrer",       IDL_USVSTRING,         false },
        { "referrerPolicy", IDL_DOMSTRING,         false },
    };

    DCHECK(g_request_rt == NULL || g_request_rt == rt,
           "Request was installed into a second runtime — its class id and step ids belong to the first, and "
           "one WASM instance is one document");
    if (g_request_rt == rt)
        return;
    g_request_rt = rt;
    JS_NewClassID(rt, &g_request_class);
    JS_NewClass(rt, g_request_class, &def);
    g_request_proto = JS_NewObject(ctx);
    CHECK(!JS_IsException(g_request_proto), "Request.prototype could not be allocated");
    idl_interface_tag(ctx, g_request_proto, "Request");
    JS_SetPropertyFunctionList(ctx, g_request_proto, js_request_proto_funcs,
                               (int)(sizeof(js_request_proto_funcs) / sizeof(js_request_proto_funcs[0])));
    g_request_body_handle = body_declare(ctx, g_request_class, request_body_of, request_body_mime, "Request");
    body_install(ctx, g_request_proto, g_request_body_handle);
    g_request_ctor_stepid = idl_method_id_step(ctx, CTOR_ARGS, 2, REQUEST_INIT,
                                               (int)(sizeof(REQUEST_INIT) / sizeof(REQUEST_INIT[0])),
                                               &js_request_ctor_decl, 0);
    idl_optional_from(1);   /* §5.3: `optional RequestInit init = {}` */
}

void request_install(JSContext *ctx, JSValueConst global)
{
    JSValue ctor;
    DCHECK(g_request_ctor_stepid >= 0, "Request was installed before request_init declared its constructor");
    ctor = idl_step_constructor(ctx, "Request", 1, g_request_ctor_stepid);
    CHECK(!JS_IsException(ctor), "the Request interface object could not be allocated");
    JS_SetConstructor(ctx, ctor, g_request_proto);
    JS_SetPropertyStr(ctx, (JSValue)global, "Request", ctor);
}

void request_free(JSContext *ctx)
{
    if (!g_request_rt)
        return;
    JS_FreeValue(ctx, g_request_proto);
    g_request_proto = JS_UNDEFINED;
    g_request_rt = NULL;
    g_request_ctor_stepid = -1;
}
