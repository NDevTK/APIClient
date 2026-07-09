/* Blob / File — see blob.h. A REAL exotic object per the Blob spec, not the old webobj opaque stub and not a
 * hand-assembled bag: a native class with an internal [[bytes]] slot (the content JSValue), a finalizer, and a
 * shared prototype (text/arrayBuffer/slice/stream) — the browser-faithful shape, like cssom.c's
 * CSSStyleDeclaration. The content is the CONCATENATED constructor parts, so `.text()`/`.arrayBuffer()` carry
 * its taint: `new Blob([location.hash]).text().then(t=>el.innerHTML=t)` and
 * `URL.createObjectURL(new Blob([html],{type:'text/html'}))` are solvable @S chains, while
 * `new Blob(['{"ep":"/x"}']).text()` returns the real JSON. An opaque part DOMINATES (returned whole so its
 * source tag survives); all-concrete parts concatenate to a real string. File is a Blob plus name/lastModified.
 * The content lives in an internal slot (never a page-visible property — that would be a fingerprintable
 * non-browser artifact); OOM while concatenating is a fatal CHECK, never a silent degrade. */
#include "blob.h"
#include <stdlib.h>
#include <string.h>
#include "idl.h"      /* the Blob interface is GENERATED from its IDL member table, not hand-assembled */
#include "opaque.h"   /* js_concolic, js_noop */
#include "check.h"    /* CHECK (OOM), DCHECK */

extern JSValue js_resolved(JSContext *ctx, JSValue val);   /* scheduler-side: wrap in a resolved promise */

static JSClassID g_blob_class_id;
typedef struct { JSValue content; } BlobData;   /* the internal [[bytes]] slot */

static void blob_finalizer(JSRuntime *rt, JSValue val) {
    BlobData *b = JS_GetOpaque(val, g_blob_class_id);
    if (b) { JS_FreeValueRT(rt, b->content); free(b); }
}

/* Concatenate the constructor parts into the content, preserving taint: an opaque part DOMINATES (return it
   whole so its source tag/example survives to the sink); all-concrete parts join to a real string. */
static JSValue blob_content_of(JSContext *ctx, int argc, JSValueConst *argv) {
    if (argc < 1) return JS_NewString(ctx, "");
    JSValueConst parts = argv[0];
    if (!JS_IsArray(parts)) return JS_IsOpaque(parts) ? JS_DupValue(ctx, parts) : JS_ToString(ctx, parts);
    uint32_t len = 0;
    JSValue lv = JS_GetPropertyStr(ctx, parts, "length"); JS_ToUint32(ctx, &len, lv); JS_FreeValue(ctx, lv);
    for (uint32_t i = 0; i < len; i++) {                 /* any opaque part -> taint dominates */
        JSValue el = JS_GetPropertyUint32(ctx, parts, i);
        if (JS_IsOpaque(el)) return el;
        JS_FreeValue(ctx, el);
    }
    if (len == 0) return JS_NewString(ctx, "");
    size_t cap = 256, o = 0; char *buf = malloc(cap);
    CHECK(buf, "Blob content allocation");                /* OOM: a dropped/degraded flow corrupts the frontier */
    for (uint32_t i = 0; i < len; i++) {                  /* all concrete: concatenate their string forms */
        JSValue el = JS_GetPropertyUint32(ctx, parts, i);
        size_t sl; const char *s = JS_ToCStringLen(ctx, &sl, el);
        if (s) {
            if (o + sl + 1 > cap) { while (o + sl + 1 > cap) cap *= 2; buf = realloc(buf, cap); CHECK(buf, "Blob content grow"); }
            memcpy(buf + o, s, sl); o += sl; JS_FreeCString(ctx, s);
        }
        JS_FreeValue(ctx, el);
    }
    JSValue out = JS_NewStringLen(ctx, buf, o); free(buf);
    return out;
}

/* Prototype methods — each operates on `this`'s internal slot. A non-Blob receiver (e.g. text.call({}), or an
   orphan-driven call with an opaque `this`) is not a should-never-happen invariant but the spec's own error
   boundary: throw TypeError exactly as the real browser does (the flow explores the throw path). */
static BlobData *blob_this(JSContext *ctx, JSValueConst t) { return JS_GetOpaque(t, g_blob_class_id); }
static JSValue m_text(JSContext *ctx, JSValueConst t, int c, JSValueConst *v) {
    (void)c; (void)v; BlobData *b = blob_this(ctx, t);
    if (!b) return JS_ThrowTypeError(ctx, "Blob.prototype.text called on non-Blob");
    return js_resolved(ctx, JS_DupValue(ctx, b->content));
}
static JSValue m_slice(JSContext *ctx, JSValueConst t, int c, JSValueConst *v) {
    (void)c; (void)v; BlobData *b = blob_this(ctx, t);
    if (!b) return JS_ThrowTypeError(ctx, "Blob.prototype.slice called on non-Blob");
    return js_blob_make(ctx, JS_DupValue(ctx, b->content), JS_UNDEFINED);   /* sub-Blob shares content (taint kept) */
}

/* interface Blob { Promise<USVString> text(); Promise<ArrayBuffer> arrayBuffer(); Blob slice(...);
   ReadableStream stream(); readonly attribute unsigned long long size; readonly attribute DOMString type; }
   — the operations are the generated prototype; size/type are per-instance readonly attributes set by make(). */
static const IDLMember BLOB_MEMBERS[] = {
    { "text",        IDL_METHOD, m_text,  0 },
    { "arrayBuffer", IDL_METHOD, m_text,  0 },
    { "slice",       IDL_METHOD, m_slice, 3 },
    { "stream",      IDL_METHOD, js_noop, 0 },
};
void blob_init(JSContext *ctx) {
    static const IDLInterface iface = { "Blob", BLOB_MEMBERS, (int)(sizeof BLOB_MEMBERS / sizeof BLOB_MEMBERS[0]), blob_finalizer };
    g_blob_class_id = idl_define_class(ctx, &iface);
}

static JSValue blob_type_opt(JSContext *ctx, JSValueConst opts) {   /* options.type or "" */
    if (!JS_IsObject(opts)) return JS_NewString(ctx, "");
    JSValue t = JS_GetPropertyStr(ctx, opts, "type");
    return JS_IsString(t) ? t : (JS_FreeValue(ctx, t), JS_NewString(ctx, ""));
}

/* Assemble a Blob object over `content` (CONSUMED) with MIME `type`. size/type are own attributes (readonly in
   the IDL); the content is the internal slot. */
JSValue js_blob_make(JSContext *ctx, JSValue content, JSValueConst type) {
    JSValue o = JS_NewObjectClass(ctx, g_blob_class_id);
    DCHECK(!JS_IsException(o), "Blob class not initialised");   /* blob_init must run in qjs_init before any construct */
    BlobData *b = malloc(sizeof *b);
    CHECK(b, "Blob slot allocation");
    b->content = content;
    JS_SetOpaque(o, b);
    JS_SetPropertyStr(ctx, o, "size", js_concolic(ctx, "{blobSize}", JS_UNDEFINED));
    JS_SetPropertyStr(ctx, o, "type", JS_IsString(type) ? JS_DupValue(ctx, type) : JS_NewString(ctx, ""));
    return o;
}

JSValue js_blob_ctor(JSContext *ctx, JSValueConst nt, int argc, JSValueConst *argv) {
    (void)nt;
    JSValue type = blob_type_opt(ctx, argc >= 2 ? argv[1] : JS_UNDEFINED);
    JSValue o = js_blob_make(ctx, blob_content_of(ctx, argc, argv), type);
    JS_FreeValue(ctx, type);
    return o;
}

/* File(bits, name, options): a Blob plus name/lastModified (File's own readonly attributes). */
JSValue js_file_ctor(JSContext *ctx, JSValueConst nt, int argc, JSValueConst *argv) {
    (void)nt;
    JSValue type = blob_type_opt(ctx, argc >= 3 ? argv[2] : JS_UNDEFINED);
    JSValue o = js_blob_make(ctx, blob_content_of(ctx, argc, argv), type);
    JS_FreeValue(ctx, type);
    JS_SetPropertyStr(ctx, o, "name", argc >= 2 && JS_IsString(argv[1]) ? JS_DupValue(ctx, argv[1]) : js_concolic(ctx, "{fileName}", JS_UNDEFINED));
    JS_SetPropertyStr(ctx, o, "lastModified", js_concolic(ctx, "{fileMtime}", JS_UNDEFINED));
    return o;
}
