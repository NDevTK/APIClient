/* The maximally-unknown concolic value + minimal host-edge stubs — see concolic.h. */
#include "solver/concolic.h"

JSValue g_concolic = JS_UNDEFINED;

void concolic_init(JSContext *ctx) { g_concolic = JS_NewConcolicShaped(ctx, "{}"); }   /* the most-general concolic (shape "{}"); generic propagation dups it */
void concolic_free(JSContext *ctx) { JS_FreeValue(ctx, g_concolic); g_concolic = JS_UNDEFINED; }

JSValue js_noop(JSContext *ctx, JSValueConst t, int c, JSValueConst *v) { return JS_UNDEFINED; }
JSValue js_concolic_read(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) { return JS_DupValue(ctx, g_concolic); }
JSValue js_concolic_stub(JSContext *ctx, JSValueConst t, int c, JSValueConst *v) { return JS_DupValue(ctx, g_concolic); }

JSValue js_concolic(JSContext *ctx, const char *shape, JSValue example) {
    JSValue o = JS_NewConcolicSourced(ctx, shape, shape);         /* forkable, source-tagged */
    if (JS_IsConcolic(o)) JS_SetConcolicExample(ctx, o, example);   /* concrete example (consumes it) */
    else JS_FreeValue(ctx, example);
    return o;
}

/* __isOpaque(v): CONCRETE bool (never forks), the ONE primitive the self-hosted Array.sort needs — a branch on an
   OPAQUE value forks, but sort must NOT fork on the meaningless ORDER of opaque elements (O(n log n) fork
   explosion). The comparator still RUNS (trampolined); only the order bit concretizes. A leaf. */
JSValue js_is_concolic(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    return JS_NewBool(ctx, argc > 0 && JS_IsConcolic(argv[0]));
}
/* __opaqueExample(v): the CONCRETE example an opaque carries (config/reply loaded data), or undefined for a pure
   attacker symbol (location.hash / cross-origin postMessage — no example). Self-hosted JSON.stringify uses it so a
   CONFIG value serializes to its real value while ATTACKER input stays the taint-preserving opaque. A leaf. */
JSValue js_concolic_example(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    return argc > 0 ? JS_ConcolicExample(ctx, argv[0]) : JS_UNDEFINED;
}
