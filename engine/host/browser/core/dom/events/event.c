/* Event / CustomEvent — see event.h. Extracted from main.c. `type` from the constructor; `detail`/`bubbles`/
 * `cancelable` from the CustomEventInit dictionary (concrete — the dispatcher chose them); an absent detail and
 * the dispatch target are opaque (external). preventDefault/stopPropagation are no-ops. */
#include "core/dom/events/event.h"
#include "solver/opaque.h"   /* g_opaque, js_noop */

JSValue js_event_ctor(JSContext *ctx, JSValueConst nt, int argc, JSValueConst *argv) {
    (void)nt;
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "type", argc >= 1 ? JS_DupValue(ctx, argv[0]) : JS_NewString(ctx, ""));
    JSValue detail = JS_UNDEFINED; int bubbles = 0, cancelable = 0;
    if (argc >= 2 && JS_IsObject(argv[1])) {
        detail = JS_GetPropertyStr(ctx, argv[1], "detail");
        JSValue b = JS_GetPropertyStr(ctx, argv[1], "bubbles"); bubbles = JS_ToBool(ctx, b); JS_FreeValue(ctx, b);
        JSValue c = JS_GetPropertyStr(ctx, argv[1], "cancelable"); cancelable = JS_ToBool(ctx, c); JS_FreeValue(ctx, c);
    }
    JS_SetPropertyStr(ctx, o, "detail", JS_IsUndefined(detail) ? js_concolic(ctx, "{eventDetail}", JS_UNDEFINED) : detail);   /* external event data -> source-tagged, not bare {} */
    JS_SetPropertyStr(ctx, o, "bubbles", JS_NewBool(ctx, bubbles));
    JS_SetPropertyStr(ctx, o, "cancelable", JS_NewBool(ctx, cancelable));
    JS_SetPropertyStr(ctx, o, "defaultPrevented", JS_FALSE);
    /* target/currentTarget are the PER-CODE-FLOW document (the real Lexbor DOM) so a handler's
       e.target.querySelector/closest/id traverses the actual per-flow document, not an opaque. */
    JSValue gg = JS_GetGlobalObject(ctx);
    JSValue doc = JS_GetPropertyStr(ctx, gg, "document");
    JS_FreeValue(ctx, gg);
    if (JS_IsObject(doc)) { JS_SetPropertyStr(ctx, o, "target", JS_DupValue(ctx, doc)); JS_SetPropertyStr(ctx, o, "currentTarget", doc); }
    else { JS_FreeValue(ctx, doc); JS_SetPropertyStr(ctx, o, "target", js_concolic(ctx, "{eventTarget}", JS_UNDEFINED)); JS_SetPropertyStr(ctx, o, "currentTarget", js_concolic(ctx, "{eventTarget}", JS_UNDEFINED)); }
    JS_SetPropertyStr(ctx, o, "preventDefault", JS_NewCFunction(ctx, js_noop, "preventDefault", 0));
    JS_SetPropertyStr(ctx, o, "stopPropagation", JS_NewCFunction(ctx, js_noop, "stopPropagation", 0));
    JS_SetPropertyStr(ctx, o, "stopImmediatePropagation", JS_NewCFunction(ctx, js_noop, "stopImmediatePropagation", 0));
    return o;
}
