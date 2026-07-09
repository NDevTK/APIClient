/* The OPAQUE sentinel + minimal host-edge stubs — see opaque.h. */
#include "opaque.h"

JSValue g_opaque = JS_UNDEFINED;

void opaque_init(JSContext *ctx) { g_opaque = JS_NewOpaqueShaped(ctx, "{}"); }   /* the default opaque (shape "{}"); generic propagation dups it */
void opaque_free(JSContext *ctx) { JS_FreeValue(ctx, g_opaque); g_opaque = JS_UNDEFINED; }

JSValue js_noop(JSContext *ctx, JSValueConst t, int c, JSValueConst *v) { return JS_UNDEFINED; }
JSValue js_opaque(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) { return JS_DupValue(ctx, g_opaque); }
JSValue js_opaque_stub(JSContext *ctx, JSValueConst t, int c, JSValueConst *v) { return JS_DupValue(ctx, g_opaque); }
