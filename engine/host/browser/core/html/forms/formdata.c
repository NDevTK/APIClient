/* FormData — see formdata.h. Extracted from main.c. append()/set() record fields into __fields; toString()
 * (shared js_sp_tostring) serializes them to "k=v&…" carrying concrete examples, so a POST body surfaces real
 * request params (a POST is never fired to learn -> the example can only come from this forced-exec serialize). */
#include "core/html/forms/formdata.h"
#include "check.h"   /* DCHECK — __fields is a ctor invariant */
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
/* get/has/delete ROUND-TRIP the recorded __fields (append/set stored them) — a stub get() dropped a
   just-appended value, the removeAttribute-did-nothing bug class. Own-property only, so inherited Object
   members (toString…) never leak into has(). __fields is a ctor invariant, so its absence is a DCHECK. */
static JSValue js_formdata_fields(JSContext *ctx, JSValueConst this_val) {
    JSValue f = JS_GetPropertyStr(ctx, this_val, "__fields");
    DCHECK(JS_IsObject(f), "FormData: __fields missing — the ctor always installs it, so this is a broken-invariant, not a page bug");
    return f;
}
static JSValue js_formdata_get(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (argc < 1) return JS_NULL;
    JSValue f = js_formdata_fields(ctx, this_val);
    const char *k = JS_ToCString(ctx, argv[0]);
    JSValue v = JS_NULL;
    if (k) { JSAtom a = JS_NewAtom(ctx, k); if (JS_GetOwnProperty(ctx, NULL, f, a) == 1) v = JS_GetProperty(ctx, f, a); JS_FreeAtom(ctx, a); JS_FreeCString(ctx, k); }
    JS_FreeValue(ctx, f);
    return v;   /* spec: null when absent */
}
static JSValue js_formdata_has(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (argc < 1) return JS_FALSE;
    JSValue f = js_formdata_fields(ctx, this_val);
    const char *k = JS_ToCString(ctx, argv[0]);
    int own = 0;
    if (k) { JSAtom a = JS_NewAtom(ctx, k); own = JS_GetOwnProperty(ctx, NULL, f, a) == 1; JS_FreeAtom(ctx, a); JS_FreeCString(ctx, k); }
    JS_FreeValue(ctx, f);
    return own ? JS_TRUE : JS_FALSE;
}
static JSValue js_formdata_delete(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (argc < 1) return JS_UNDEFINED;
    JSValue f = js_formdata_fields(ctx, this_val);
    const char *k = JS_ToCString(ctx, argv[0]);
    if (k) { JSAtom a = JS_NewAtom(ctx, k); JS_DeleteProperty(ctx, f, a, 0); JS_FreeAtom(ctx, a); JS_FreeCString(ctx, k); }
    JS_FreeValue(ctx, f);
    return JS_UNDEFINED;
}
JSValue js_formdata_ctor(JSContext *ctx, JSValueConst nt, int argc, JSValueConst *argv) {
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "__fields", JS_NewObject(ctx));
    JS_SetPropertyStr(ctx, o, "append", JS_NewCFunction(ctx, js_formdata_append, "append", 2));
    JS_SetPropertyStr(ctx, o, "set", JS_NewCFunction(ctx, js_formdata_append, "set", 2));
    JS_SetPropertyStr(ctx, o, "get", JS_NewCFunction(ctx, js_formdata_get, "get", 1));
    JS_SetPropertyStr(ctx, o, "has", JS_NewCFunction(ctx, js_formdata_has, "has", 1));
    JS_SetPropertyStr(ctx, o, "delete", JS_NewCFunction(ctx, js_formdata_delete, "delete", 1));
    JS_SetPropertyStr(ctx, o, "toString", JS_NewCFunction(ctx, js_sp_tostring, "toString", 0));
    return o;
}
