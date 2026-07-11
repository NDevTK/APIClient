/* Encoding — see encoding.h. Real, IDL-faithful TextEncoder/TextDecoder (replacing the generic js_webobj stub
 * whose TextEncoder wrongly had .json()/.getReader() and returned bare opaque). encode(str): a CONCRETE string
 * -> a real Uint8Array of its UTF-8 bytes (nothing opaque); a TAINTED/opaque string -> a source-tagged concolic
 * `{bytes}` carrying the input as its example (the taint flows, @S sees it), never the bare `{}` sentinel.
 * decode(bytes): a concrete Uint8Array/ArrayBuffer -> its UTF-8 string; opaque -> `{text}` concolic. */
#include "modules/encoding.h"
#include "solver/opaque.h"   /* g_opaque, js_concolic */

static JSValue enc_encode(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) return JS_NewUint8ArrayCopy(ctx, (const uint8_t *)"", 0);
    if (JS_IsConcolic(argv[0])) return js_concolic(ctx, "{bytes}", JS_ConcolicExample(ctx, argv[0]));   /* tainted -> concolic bytes, not bare opaque */
    size_t len = 0; const char *s = JS_ToCStringLen(ctx, &len, argv[0]);
    JSValue r = JS_NewUint8ArrayCopy(ctx, (const uint8_t *)(s ? s : ""), s ? len : 0);   /* real UTF-8 bytes */
    if (s) JS_FreeCString(ctx, s);
    return r;
}
static JSValue dec_decode(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) return JS_NewString(ctx, "");
    if (JS_IsConcolic(argv[0])) return js_concolic(ctx, "{text}", JS_ConcolicExample(ctx, argv[0]));   /* decoding unknown bytes -> concolic text */
    size_t size = 0; uint8_t *buf = JS_GetUint8Array(ctx, &size, argv[0]);
    if (!buf) { JSValue e = JS_GetException(ctx); JS_FreeValue(ctx, e); buf = JS_GetArrayBuffer(ctx, &size, argv[0]); }
    if (!buf) { JSValue e = JS_GetException(ctx); JS_FreeValue(ctx, e); return js_concolic(ctx, "{text}", JS_UNDEFINED); }
    return JS_NewStringLen(ctx, (const char *)buf, size);   /* real UTF-8 decode */
}

JSValue js_textencoder_ctor(JSContext *ctx, JSValueConst nt, int argc, JSValueConst *argv) {
    (void)nt; (void)argc; (void)argv;
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "encode", JS_NewCFunction(ctx, enc_encode, "encode", 1));
    JS_SetPropertyStr(ctx, o, "encoding", JS_NewString(ctx, "utf-8"));
    return o;
}
JSValue js_textdecoder_ctor(JSContext *ctx, JSValueConst nt, int argc, JSValueConst *argv) {
    (void)nt; (void)argc; (void)argv;
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "decode", JS_NewCFunction(ctx, dec_decode, "decode", 1));
    JS_SetPropertyStr(ctx, o, "encoding", JS_NewString(ctx, "utf-8"));
    return o;
}
