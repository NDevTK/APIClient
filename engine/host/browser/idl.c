/* Web IDL binding driver — see idl.h. A declarative IDL member table becomes a native object: operations are
 * functions, opaque attributes are getters returning the concolic unknown. A headless-unknown readonly
 * attribute still EXISTS and has its type (the IDL declares it), but its VALUE forks — spec-locked shape,
 * honest-unknown value. The interface reads as its IDL, never hand-assembled property-by-property. */
#include "idl.h"
#include "opaque.h"   /* g_opaque — the concolic unknown an opaque attribute reads as */

/* A getter's calling convention is (ctx, this_val) — NOT the 4-arg JSCFunction shape; on wasm a mismatched
   call_indirect signature traps, so this MUST be the true getter signature (cast to JSCFunction at install). */
static JSValue idl_opaque_getter(JSContext *ctx, JSValueConst this_val) {
    (void)this_val; return JS_DupValue(ctx, g_opaque);
}

JSValue idl_instance(JSContext *ctx, const IDLMember *members, int n) {
    JSValue o = JS_NewObject(ctx);
    for (int i = 0; i < n; i++) {
        const IDLMember *m = &members[i];
        if (m->kind == IDL_METHOD) {
            JS_SetPropertyStr(ctx, o, m->name, JS_NewCFunction(ctx, m->fn, m->name, m->length));
        } else {   /* IDL_ATTR_OPAQUE — a readonly attribute whose value is unknown headless (getter -> forks) */
            JSAtom a = JS_NewAtom(ctx, m->name);
            JS_DefinePropertyGetSet(ctx, o, a,
                JS_NewCFunction2(ctx, (JSCFunction *)idl_opaque_getter, m->name, 0, JS_CFUNC_getter, 0),
                JS_UNDEFINED, JS_PROP_CONFIGURABLE);
            JS_FreeAtom(ctx, a);
        }
    }
    return o;
}
