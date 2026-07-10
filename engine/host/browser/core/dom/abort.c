/* AbortSignal — see abort.h. Built from its Web IDL via the idl.h driver, not hand-assembled:
 *
 *   interface AbortSignal : EventTarget {
 *     readonly attribute boolean aborted;      // unknown headless -> forks (never a fixed `false` that
 *     readonly attribute any     reason;       //   silently takes only the not-aborted arm)
 *     undefined throwIfAborted();
 *     // EventTarget: addEventListener / removeEventListener
 *   };
 *
 * The static factories AbortSignal.timeout/any/abort all yield an instance of this interface. */
#include "core/dom/abort.h"
#include "bindings/idl.h"
#include "check.h"    /* DCHECK — throwIfAborted's self-hosted bytecode is guaranteed-valid; a compile failure is a should-never-happen */
#include "opaque.h"   /* js_noop */

extern JSValue js_add_listener(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv);   /* EventTarget.addEventListener -> driven flow */

static const IDLMember ABORTSIGNAL_IDL[] = {
    { "aborted",             IDL_ATTR_OPAQUE, NULL,            0 },
    { "reason",              IDL_ATTR_OPAQUE, NULL,            0 },
    { "addEventListener",    IDL_METHOD,      js_add_listener, 2 },
    { "removeEventListener", IDL_METHOD,      js_noop,         2 },
};

JSValue js_abortsignal_make(JSContext *ctx, JSValueConst t, int c, JSValueConst *v) {
    (void)t; (void)c; (void)v;
    JSValue o = idl_instance(ctx, ABORTSIGNAL_IDL, sizeof ABORTSIGNAL_IDL / sizeof ABORTSIGNAL_IDL[0]);
    /* throwIfAborted() is SELF-HOSTED as bytecode (not a C no-op) so the `if (this.aborted)` branches on the
       concolic `aborted` at the OPCODE level — forking throw-vs-continue (a C `if` on JS_ToBool can't fork).
       On the aborted arm it throws the concolic `reason`, so a try/catch/.catch path (which can itself reach a
       sink) is explored; the not-aborted arm returns undefined. */
    static const char SRC[] = "(function(){ if (this.aborted) throw this.reason; })";
    JSValue fn = JS_Eval(ctx, SRC, sizeof SRC - 1, "<AbortSignal.throwIfAborted>", JS_EVAL_TYPE_GLOBAL);
    DCHECK(!JS_IsException(fn), "throwIfAborted self-host failed to compile — guaranteed-valid bytecode");
    JS_SetPropertyStr(ctx, o, "throwIfAborted", fn);
    return o;
}

/* interface AbortController { constructor(); readonly attribute AbortSignal signal; undefined abort(reason); }
   .signal is a REAL AbortSignal (its aborted attr is concolic -> a gate forks both worlds), not the old
   generic-webobj opaque signal; abort() is a no-op (the fork already explores the aborted arm). */
JSValue js_abortcontroller_ctor(JSContext *ctx, JSValueConst nt, int argc, JSValueConst *argv) {
    (void)nt; (void)argc; (void)argv;
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "signal", js_abortsignal_make(ctx, JS_UNDEFINED, 0, NULL));
    JS_SetPropertyStr(ctx, o, "abort", JS_NewCFunction(ctx, js_noop, "abort", 1));
    return o;
}
