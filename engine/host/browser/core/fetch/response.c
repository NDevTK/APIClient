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
#include "core/fetch/response.h"

static JSClassID g_response_class;

typedef struct { char *url; char *body; int body_used; } ResponseData;

static void response_finalizer(JSRuntime *rt, JSValue val)
{
    ResponseData *d = JS_GetOpaque(val, g_response_class);
    (void)rt;
    if (d) { js_free_rt(rt, d->url); js_free_rt(rt, d->body); js_free_rt(rt, d); }
}

static ResponseData *response_of(JSValueConst v) { return JS_GetOpaque(v, g_response_class); }

/* 6.4 "consume body": the latch is per Response and the second read is a TypeError — a page's retry path tests
   exactly that, so answering the body twice would hide the branch it takes. */
static JSValue response_take_body(JSContext *ctx, JSValueConst this_val, const char **pbody)
{
    ResponseData *d = response_of(this_val);
    if (!d)
        return JS_ThrowTypeError(ctx, "not a Response");
    if (d->body_used)
        return JS_ThrowTypeError(ctx, "body stream already read");
    d->body_used = 1;
    *pbody = d->body ? d->body : "";
    return JS_UNDEFINED;
}

/* A settled promise, because the bytes are already here: the host fetched them before this Response existed. */
static JSValue response_settled(JSContext *ctx, JSValue value, int reject)
{
    JSValue promise, funcs[2], r;
    promise = JS_NewPromiseCapability(ctx, funcs);
    if (JS_IsException(promise)) { JS_FreeValue(ctx, value); return promise; }
    /* Resolving with an OBJECT reads its `then` — the page's code — so this would be a C-driven trap the
       moment a body value stopped being a primitive. It never is (the bytes are a string the host already
       fetched), and saying so as an assert is what keeps this call from quietly becoming one. */
    DCHECK(JS_VALUE_GET_TAG(value) != JS_TAG_OBJECT,
           "a Response settled with an OBJECT — resolving one reads its `then` from C with no flow base; "
           "route this through the promise machinery instead");
    r = JS_Call(ctx, funcs[reject ? 1 : 0], JS_UNDEFINED, 1, (JSValueConst *)&value);
    JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, value);
    JS_FreeValue(ctx, funcs[0]);
    JS_FreeValue(ctx, funcs[1]);
    return promise;
}

static JSValue js_response_text(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    const char *body = NULL;
    JSValue err = response_take_body(ctx, this_val, &body);
    (void)argc; (void)argv;
    if (JS_IsException(err))
        return response_settled(ctx, JS_GetException(ctx), 1);
    return response_settled(ctx, JS_NewString(ctx, body), 0);
}

/* 6.4.3 json(): parse the body with the REAL parser, so a malformed body rejects with the SyntaxError the page
   would actually catch rather than a placeholder this engine invented. */
static JSValue js_response_json(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    const char *body = NULL;
    JSValue err = response_take_body(ctx, this_val, &body), parsed;
    (void)argc; (void)argv;
    if (JS_IsException(err))
        return response_settled(ctx, JS_GetException(ctx), 1);
    parsed = JS_ParseJSON(ctx, body, strlen(body), "<response>");
    if (JS_IsException(parsed))
        return response_settled(ctx, JS_GetException(ctx), 1);
    return response_settled(ctx, parsed, 0);
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
    JS_CFUNC_DEF("text", 0, js_response_text),
    JS_CFUNC_DEF("json", 0, js_response_json),
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
    JS_NewClassID(JS_GetRuntime(ctx), &g_response_class);
    JS_NewClass(JS_GetRuntime(ctx), g_response_class, &def);
}

JSValue response_new(JSContext *ctx, const char *url, const char *body)
{
    ResponseData *d;
    JSValue obj;

    DCHECK(g_response_class != 0, "a Response was built before the class existed — response_init runs at install");
    obj = JS_NewObjectClass(ctx, g_response_class);
    if (JS_IsException(obj))
        return obj;
    d = js_mallocz(ctx, sizeof(*d));
    if (!d) { JS_FreeValue(ctx, obj); return JS_EXCEPTION; }
    d->url = js_strdup(ctx, url ? url : "");
    d->body = js_strdup(ctx, body ? body : "");
    CHECK(d->url && d->body, "response: OOM copying a reply — a dropped body loses everything behind the request");
    JS_SetOpaque(obj, d);
    JS_SetPropertyFunctionList(ctx, obj, js_response_proto,
                               (int)(sizeof(js_response_proto) / sizeof(js_response_proto[0])));
    return obj;
}
