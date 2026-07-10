/* FormData — see formdata.h. Extracted from main.c. append()/set() record fields into __fields; toString()
 * (shared js_sp_tostring) serializes them to "k=v&…" carrying concrete examples, so a POST body surfaces real
 * request params (a POST is never fired to learn -> the example can only come from this forced-exec serialize). */
#include "core/html/forms/formdata.h"
#include "opaque.h"   /* js_noop, js_opaque_stub */
#include "platform/urlobj.h"   /* js_sp_tostring — the concolic query serializer, shared with URLSearchParams */

static JSValue js_formdata_append(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (argc >= 2) {
        JSValue f = JS_GetPropertyStr(ctx, this_val, "__fields");
        if (!JS_IsObject(f)) { JS_FreeValue(ctx, f); f = JS_NewObject(ctx); JS_SetPropertyStr(ctx, this_val, "__fields", JS_DupValue(ctx, f)); }
        const char *k = JS_ToCString(ctx, argv[0]);
        if (k) { JS_SetPropertyStr(ctx, f, k, JS_DupValue(ctx, argv[1])); JS_FreeCString(ctx, k); }
        JS_FreeValue(ctx, f);
    }
    return JS_UNDEFINED;
}
JSValue js_formdata_ctor(JSContext *ctx, JSValueConst nt, int argc, JSValueConst *argv) {
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "__fields", JS_NewObject(ctx));
    JS_SetPropertyStr(ctx, o, "append", JS_NewCFunction(ctx, js_formdata_append, "append", 2));
    JS_SetPropertyStr(ctx, o, "set", JS_NewCFunction(ctx, js_formdata_append, "set", 2));
    JS_SetPropertyStr(ctx, o, "get", JS_NewCFunction(ctx, js_opaque_stub, "get", 1));
    JS_SetPropertyStr(ctx, o, "has", JS_NewCFunction(ctx, js_opaque_stub, "has", 1));
    JS_SetPropertyStr(ctx, o, "delete", JS_NewCFunction(ctx, js_noop, "delete", 1));
    JS_SetPropertyStr(ctx, o, "toString", JS_NewCFunction(ctx, js_sp_tostring, "toString", 0));
    return o;
}
