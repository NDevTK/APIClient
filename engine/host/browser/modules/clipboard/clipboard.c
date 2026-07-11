/* Clipboard module — see clipboard.h. */
#include "modules/clipboard/clipboard.h"
#include "bindings/idl.h"        /* idl_dfail_wrap — unbuilt Clipboard members DFAIL, never an opaque shrug */
#include "solver/concolic.h"     /* js_noop, JS_NewConcolicSourced */
#include "solver/source.h"       /* source_candidate — clipboard is an attacker source, delivered raw */

#include "platform/promise.h"
extern JSValue js_add_listener(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv);       /* 'clipboardchange' -> driven flow */

static JSValue clip_promise_undef(JSContext *ctx, JSValueConst t, int c, JSValueConst *v) { (void)t; (void)c; (void)v; return js_resolved(ctx, JS_UNDEFINED); }

/* readText()/read(): ATTACKER-CONTROLLED paste content — a real XSS SOURCE ({clipboard}). Under an @S replay the
   candidate breakout is delivered raw (no browser transform encodes clipboard text); otherwise a concolic source
   whose gate still forks and whose taint reaches the sink. */
static JSValue clip_read(JSContext *ctx, JSValueConst t, int c, JSValueConst *v) {
    (void)t; (void)c; (void)v;
    { JSValue cd = source_candidate(ctx, "", 0, 0, 0); if (!JS_IsUndefined(cd)) return js_resolved(ctx, cd); }
    return js_resolved(ctx, JS_NewConcolicSourced(ctx, "{clipboard}", "{clipboard}"));
}

JSValue clipboard_make(JSContext *ctx) {
    JSValue cb = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, cb, "readText", JS_NewCFunction(ctx, clip_read, "readText", 0));
    JS_SetPropertyStr(ctx, cb, "read", JS_NewCFunction(ctx, clip_read, "read", 0));
    JS_SetPropertyStr(ctx, cb, "writeText", JS_NewCFunction(ctx, clip_promise_undef, "writeText", 1));   /* not a scriptable sink */
    JS_SetPropertyStr(ctx, cb, "write", JS_NewCFunction(ctx, clip_promise_undef, "write", 1));
    JS_SetPropertyStr(ctx, cb, "addEventListener", JS_NewCFunction(ctx, js_add_listener, "addEventListener", 2));
    JS_SetPropertyStr(ctx, cb, "removeEventListener", JS_NewCFunction(ctx, js_noop, "removeEventListener", 2));
    return idl_dfail_wrap(ctx, cb, "Clipboard");   /* unbuilt Clipboard members DFAIL */
}
