/* Response — see response.h. A REAL exotic class with an internal [[body]] slot (like Blob), generated from
 * its IDL member table, replacing the generic webobj opaque stub. The body is the constructor argument, so
 * `new Response(JSON.stringify(cfg)).json().then(c=>fetch(c.ep))` reads the real config and
 * `new Response(location.hash).text().then(t=>el.innerHTML=t)` carries the taint. status/ok/statusText/headers
 * come from the init dict. (A FETCH response is different — its body is the server reply, built by reply.c.) */
#include "core/loader/response.h"
#include "bindings/idl.h"      /* generated from the IDL member table */
#include "solver/concolic.h"   /* js_concolic, js_noop */
#include "check.h"    /* CHECK (OOM) */

extern JSValue js_resolved(JSContext *ctx, JSValue val);   /* scheduler-side: wrap in a resolved promise */
extern JSValue js_rejected(JSContext *ctx, JSValue err);   /* scheduler-side: a rejected promise (re-throws into the await) */
extern JSValue js_headers_ctor(JSContext *ctx, JSValueConst nt, int argc, JSValueConst *argv);   /* real Headers (urlobj.c) */

static JSClassID g_resp_class_id;
typedef struct { JSValue body; } RespData;   /* the internal [[body]] slot */

static void resp_finalizer(JSRuntime *rt, JSValue val) {
    RespData *r = JS_GetOpaque(val, g_resp_class_id);
    if (r) { JS_FreeValueRT(rt, r->body); free(r); }
}
static RespData *resp_this(JSContext *ctx, JSValueConst t) { return JS_GetOpaque(t, g_resp_class_id); }

/* text(): the body as text (a string stays itself; an opaque/tainted body carries through). */
static JSValue m_text(JSContext *ctx, JSValueConst t, int c, JSValueConst *v) {
    (void)c; (void)v; RespData *r = resp_this(ctx, t);
    if (!r) return JS_ThrowTypeError(ctx, "Response.prototype.text called on non-Response");
    return js_resolved(ctx, JS_DupValue(ctx, r->body));
}
/* json(): parse a concrete-string body; an opaque body passes through (taint preserved, never a fake concrete). */
static JSValue m_json(JSContext *ctx, JSValueConst t, int c, JSValueConst *v) {
    (void)c; (void)v; RespData *r = resp_this(ctx, t);
    if (!r) return JS_ThrowTypeError(ctx, "Response.prototype.json called on non-Response");
    if (JS_IsString(r->body)) {
        size_t len; const char *s = JS_ToCStringLen(ctx, &len, r->body);
        JSValue parsed = s ? JS_ParseJSON(ctx, s, len, "<response>") : JS_UNDEFINED;
        if (s) JS_FreeCString(ctx, s);
        if (JS_IsException(parsed)) return js_rejected(ctx, JS_GetException(ctx));   /* malformed body -> REJECT (spec), not a fake concolic that hides the .catch path */
        return js_resolved(ctx, parsed);
    }
    return js_resolved(ctx, JS_DupValue(ctx, r->body));   /* opaque/tainted body: keep it */
}

/* interface Response { Promise<any> json(); Promise<USVString> text(); Promise<Blob> blob();
   Promise<ArrayBuffer> arrayBuffer(); Response clone(); ... } — operations generated onto the prototype. */
static JSValue m_self(JSContext *ctx, JSValueConst t, int c, JSValueConst *v) { (void)c; (void)v; return JS_DupValue(ctx, t); }
static const IDLMember RESP_MEMBERS[] = {
    { "json",        IDL_METHOD, m_json, 0 },
    { "text",        IDL_METHOD, m_text, 0 },
    { "blob",        IDL_METHOD, m_text, 0 },   /* body-as-Blob: taint-equivalent to text for solver purposes */
    { "arrayBuffer", IDL_METHOD, m_text, 0 },
    { "bytes",       IDL_METHOD, m_text, 0 },   /* Promise<Uint8Array>: body bytes, taint-equivalent to text */
    { "formData",    IDL_METHOD, m_text, 0 },
    { "clone",       IDL_METHOD, m_self, 0 },
};
void response_init(JSContext *ctx) {
    static const IDLInterface iface = { "Response", RESP_MEMBERS, (int)(sizeof RESP_MEMBERS / sizeof RESP_MEMBERS[0]), resp_finalizer };
    g_resp_class_id = idl_define_class(ctx, &iface);
}

static int init_int(JSContext *ctx, JSValueConst init, const char *k, int dflt) {
    if (!JS_IsObject(init)) return dflt;
    JSValue v = JS_GetPropertyStr(ctx, init, k); int out = dflt;
    if (!JS_IsUndefined(v)) JS_ToInt32(ctx, &out, v);
    JS_FreeValue(ctx, v); return out;
}

JSValue js_response_ctor(JSContext *ctx, JSValueConst nt, int argc, JSValueConst *argv) {
    (void)nt;
    JSValue o = JS_NewObjectClass(ctx, g_resp_class_id);
    RespData *r = malloc(sizeof *r);
    CHECK(r, "Response slot allocation");
    r->body = argc >= 1 ? JS_DupValue(ctx, argv[0]) : JS_NewString(ctx, "");   /* the [[body]] = ctor arg */
    JS_SetOpaque(o, r);
    JSValueConst init = argc >= 2 ? argv[1] : JS_UNDEFINED;
    int status = init_int(ctx, init, "status", 200);
    JS_SetPropertyStr(ctx, o, "status", JS_NewInt32(ctx, status));
    JS_SetPropertyStr(ctx, o, "ok", JS_NewBool(ctx, status >= 200 && status < 300));
    JS_SetPropertyStr(ctx, o, "statusText", JS_NewString(ctx, ""));
    JS_SetPropertyStr(ctx, o, "bodyUsed", JS_FALSE);
    /* A constructor-built (synthetic) Response has, per the Fetch spec, an EMPTY url, type "default", and is not
       redirected — real spec-defined scalars (a fetch response overrides these in reply.c), never a stub. */
    JS_SetPropertyStr(ctx, o, "url", JS_NewString(ctx, ""));
    JS_SetPropertyStr(ctx, o, "type", JS_NewString(ctx, "default"));
    JS_SetPropertyStr(ctx, o, "redirected", JS_FALSE);
    /* headers: a page-constructed Response's headers are KNOWN app data (init.headers), not unknowable input —
       build a real Headers so `new Response(b,{headers:{...}}).headers.get(k)` returns the set value. */
    JSValue ih = JS_IsObject(init) ? JS_GetPropertyStr(ctx, init, "headers") : JS_UNDEFINED;
    JS_SetPropertyStr(ctx, o, "headers", js_headers_ctor(ctx, JS_UNDEFINED, JS_IsObject(ih) ? 1 : 0, (JSValueConst *)&ih));
    JS_FreeValue(ctx, ih);
    return o;
}
