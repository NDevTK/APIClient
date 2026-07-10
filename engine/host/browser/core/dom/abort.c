/* AbortSignal / AbortController — Blink core/dom. A faithful hand implementation: every member is a REAL impl
 * or a dedicated, documented headless behavior — never a generic noop stub. The canonical AbortSignal IDL is
 * used to AUDIT this for missing members (engine/idlgen.mjs), not to generate stub scaffolding. */
#include "core/dom/abort.h"
#include "check.h"    /* DCHECK — throwIfAborted's self-hosted bytecode is guaranteed-valid */
#include "opaque.h"   /* js_concolic — aborted/reason are genuinely unknown headless (concolic: forks, provenance) */

extern JSValue js_add_listener(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv);   /* register a handler -> driven flow */

/* aborted / reason are genuinely UNKNOWN headless (no controller has aborted): the concolic value forks a gate
   on them so BOTH the aborted and not-aborted worlds are explored — the faithful headless value, not a stub. */
static JSValue as_get_aborted(JSContext *ctx, JSValueConst t) { (void)t; return js_concolic(ctx, "{aborted}", JS_UNDEFINED); }
static JSValue as_get_reason (JSContext *ctx, JSValueConst t) { (void)t; return js_concolic(ctx, "{reason}",  JS_UNDEFINED); }
/* addEventListener registers the handler as a driven flow. remove/dispatch are DELIBERATE no-effect for analysis
   (documented): we keep every registered handler REACHABLE for orphan-driving rather than honour removal, and we
   drive handlers by exploration rather than a synthetic dispatch. */
static JSValue as_removeEventListener(JSContext *ctx, JSValueConst t, int c, JSValueConst *v) { (void)ctx; (void)t; (void)c; (void)v; return JS_UNDEFINED; }
static JSValue as_dispatchEvent      (JSContext *ctx, JSValueConst t, int c, JSValueConst *v) { (void)ctx; (void)t; (void)c; (void)v; return JS_FALSE; }
/* onabort = fn : register `fn` for the 'abort' event (like addEventListener('abort', fn)) so it is driven. */
static JSValue as_set_onabort(JSContext *ctx, JSValueConst t, JSValueConst val) {
    if (JS_IsFunction(ctx, val)) { JSValue ty = JS_NewString(ctx, "abort"); JSValueConst a[2] = { ty, val };
        JSValue r = js_add_listener(ctx, t, 2, a); JS_FreeValue(ctx, r); JS_FreeValue(ctx, ty); }
    return JS_UNDEFINED;
}
typedef JSValue (*GetFn)(JSContext *, JSValueConst);
typedef JSValue (*SetFn)(JSContext *, JSValueConst, JSValueConst);
static void def_getset(JSContext *ctx, JSValue o, const char *name, GetFn get, SetFn set) {
    JSAtom a = JS_NewAtom(ctx, name);
    JS_DefinePropertyGetSet(ctx, o, a,
        JS_NewCFunction2(ctx, (JSCFunction *)get, name, 0, JS_CFUNC_getter, 0),
        set ? JS_NewCFunction2(ctx, (JSCFunction *)set, name, 1, JS_CFUNC_setter, 0) : JS_UNDEFINED,
        JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
    JS_FreeAtom(ctx, a);
}

JSValue js_abortsignal_make(JSContext *ctx, JSValueConst t, int c, JSValueConst *v) {
    (void)t; (void)c; (void)v;
    JSValue o = JS_NewObject(ctx);
    def_getset(ctx, o, "aborted", as_get_aborted, NULL);
    def_getset(ctx, o, "reason",  as_get_reason,  NULL);
    def_getset(ctx, o, "onabort", as_get_reason,  as_set_onabort);   /* onabort read is opaque; write registers the handler */
    JS_SetPropertyStr(ctx, o, "addEventListener",    JS_NewCFunction(ctx, js_add_listener, "addEventListener", 2));
    JS_SetPropertyStr(ctx, o, "removeEventListener", JS_NewCFunction(ctx, as_removeEventListener, "removeEventListener", 2));
    JS_SetPropertyStr(ctx, o, "dispatchEvent",       JS_NewCFunction(ctx, as_dispatchEvent, "dispatchEvent", 1));
    /* throwIfAborted() is SELF-HOSTED bytecode so `if (this.aborted) throw this.reason` branches on the concolic
       `aborted` at the OPCODE level — forking throw-vs-continue (a C `if` on JS_ToBool cannot fork). */
    static const char SRC[] = "(function(){ if (this.aborted) throw this.reason; })";
    JSValue fn = JS_Eval(ctx, SRC, sizeof SRC - 1, "<AbortSignal.throwIfAborted>", JS_EVAL_TYPE_GLOBAL);
    DCHECK(!JS_IsException(fn), "throwIfAborted self-host failed to compile — guaranteed-valid bytecode");
    JS_SetPropertyStr(ctx, o, "throwIfAborted", fn);
    return o;
}

/* AbortController { readonly attribute AbortSignal signal; undefined abort(reason); }. abort() is a documented
   no-effect: `aborted` is already concolic so the aborted world is explored regardless — a conscious decision. */
static JSValue ac_abort(JSContext *ctx, JSValueConst t, int c, JSValueConst *v) { (void)ctx; (void)t; (void)c; (void)v; return JS_UNDEFINED; }
JSValue js_abortcontroller_ctor(JSContext *ctx, JSValueConst nt, int argc, JSValueConst *argv) {
    (void)nt; (void)argc; (void)argv;
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "signal", js_abortsignal_make(ctx, JS_UNDEFINED, 0, NULL));
    JS_SetPropertyStr(ctx, o, "abort", JS_NewCFunction(ctx, ac_abort, "abort", 1));
    return o;
}
