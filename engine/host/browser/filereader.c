/* FileReader — see filereader.h. Extracted from the generic webobj stub and made real: readAsText/
 * readAsDataURL/readAsArrayBuffer set reader.result to the file's ATTACKER-controlled content (a concolic
 * {fileContent} source; a replay flow injects the @S candidate) and fire onload, so the classic file-upload
 * XSS — `r.onload=function(){el.innerHTML=r.result}; r.readAsText(f)` — is a solvable @S chain: the onload
 * handler (driven as a flow) reads reader.result (via closure or e.target.result) and sinks the content. */
#include "filereader.h"
#include "opaque.h"   /* js_concolic, js_noop */

extern JSValue js_add_listener(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv);
extern char *g_candidate;   /* @S replay: the file content is attacker-controlled -> deliver the candidate */

static JSValue fr_content(JSContext *ctx) {
    if (g_candidate) return JS_NewString(ctx, g_candidate);
    return js_concolic(ctx, "{fileContent}", JS_UNDEFINED);
}

/* readAsText/readAsDataURL/readAsArrayBuffer(file): set reader.result to the content, then fire onload +
   onloadend. This is a CONTINUATION of the CURRENT flow, NOT a fresh orphan: the read completed inside this
   flow's COW delta, so onload must run on the LIVE delta where reader.result is set (a deferred fresh-baseline
   flow would fork from the pristine boot state and see reader.result = null — the taint would vanish). So fire
   onload IN-FLOW via a direct call, delivering a ProgressEvent whose target/currentTarget is the reader (so a
   handler reading e.target.result gets the content just as one reading the closed-over reader.result does). */
static JSValue fr_read(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)argc; (void)argv;
    JS_SetPropertyStr(ctx, this_val, "result", fr_content(ctx));
    JS_SetPropertyStr(ctx, this_val, "readyState", JS_NewInt32(ctx, 2));   /* DONE */
    JSValue ev = JS_NewObject(ctx);                                        /* ProgressEvent (target = the reader) */
    JS_SetPropertyStr(ctx, ev, "type", JS_NewString(ctx, "load"));
    JS_SetPropertyStr(ctx, ev, "target", JS_DupValue(ctx, this_val));
    JS_SetPropertyStr(ctx, ev, "currentTarget", JS_DupValue(ctx, this_val));
    for (int i = 0; i < 2; i++) {   /* fire onload then onloadend, in-flow, on the live delta */
        JSValue cb = JS_GetPropertyStr(ctx, this_val, i ? "onloadend" : "onload");
        if (JS_IsFunction(ctx, cb)) JS_FreeValue(ctx, JS_Call(ctx, cb, this_val, 1, (JSValueConst *)&ev));
        JS_FreeValue(ctx, cb);
    }
    JS_FreeValue(ctx, ev);
    return JS_UNDEFINED;
}

JSValue js_filereader_ctor(JSContext *ctx, JSValueConst nt, int argc, JSValueConst *argv) {
    (void)nt; (void)argc; (void)argv;
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "result", JS_NULL);   /* null until a read completes */
    JS_SetPropertyStr(ctx, o, "error", JS_NULL);
    JS_SetPropertyStr(ctx, o, "readyState", JS_NewInt32(ctx, 0));   /* EMPTY */
    JS_SetPropertyStr(ctx, o, "readAsText", JS_NewCFunction(ctx, fr_read, "readAsText", 2));
    JS_SetPropertyStr(ctx, o, "readAsDataURL", JS_NewCFunction(ctx, fr_read, "readAsDataURL", 1));
    JS_SetPropertyStr(ctx, o, "readAsArrayBuffer", JS_NewCFunction(ctx, fr_read, "readAsArrayBuffer", 1));
    JS_SetPropertyStr(ctx, o, "readAsBinaryString", JS_NewCFunction(ctx, fr_read, "readAsBinaryString", 1));
    JS_SetPropertyStr(ctx, o, "abort", JS_NewCFunction(ctx, js_noop, "abort", 0));
    JS_SetPropertyStr(ctx, o, "addEventListener", JS_NewCFunction(ctx, js_add_listener, "addEventListener", 2));
    JS_SetPropertyStr(ctx, o, "removeEventListener", JS_NewCFunction(ctx, js_noop, "removeEventListener", 2));
    JS_SetPropertyStr(ctx, o, "onload", JS_NULL);
    JS_SetPropertyStr(ctx, o, "onloadend", JS_NULL);
    JS_SetPropertyStr(ctx, o, "onerror", JS_NULL);
    return o;
}
