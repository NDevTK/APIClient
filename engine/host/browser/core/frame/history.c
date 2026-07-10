/* History — see history.h. Extracted from main.c and modeled as a real state machine instead of a noop
 * pushState + opaque state (headless is not valueless — the History state is DEFINED without a rendered
 * navigation): pushState/replaceState(data, unused, url) SET history.state to `data`, so
 * `history.replaceState(cfg,'',u); … if (history.state.role==='admin') …` reads the REAL stashed value. The
 * state mutation lands on the history object -> per-flow via the heap COW, like any other write. back/forward/
 * go have no headless-observable effect (no rendered navigation) so they stay no-ops, but the state they do
 * NOT change is faithful (null until the page pushes one — spec, never an opaque shrug). */
#include "core/frame/history.h"

static JSValue js_history_set_state(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    JS_SetPropertyStr(ctx, this_val, "state", argc >= 1 ? JS_DupValue(ctx, argv[0]) : JS_NULL);   /* current entry's state = data */
    return JS_UNDEFINED;
}

/* back()/forward()/go(delta): history traversal. Headless there is no rendered navigation and we keep only the
   current entry (no back-stack to move within), so there is nothing to navigate TO; the popstate handlers a
   traversal would fire are already REACHABLE via orphan-driving (registered addEventListener flows), so this is
   a DEDICATED, documented no-effect — not the generic js_noop stub the audit exists to expose. */
static JSValue js_history_traverse(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)ctx; (void)this_val; (void)argc; (void)argv; return JS_UNDEFINED;
}

JSValue js_history_make(JSContext *ctx) {
    JSValue h = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, h, "state", JS_NULL);                         /* no navigation yet -> null (spec), not opaque */
    JS_SetPropertyStr(ctx, h, "length", JS_NewInt32(ctx, 1));
    JS_SetPropertyStr(ctx, h, "scrollRestoration", JS_NewString(ctx, "auto"));
    JS_SetPropertyStr(ctx, h, "pushState", JS_NewCFunction(ctx, js_history_set_state, "pushState", 3));
    JS_SetPropertyStr(ctx, h, "replaceState", JS_NewCFunction(ctx, js_history_set_state, "replaceState", 3));
    JS_SetPropertyStr(ctx, h, "back", JS_NewCFunction(ctx, js_history_traverse, "back", 0));
    JS_SetPropertyStr(ctx, h, "forward", JS_NewCFunction(ctx, js_history_traverse, "forward", 0));
    JS_SetPropertyStr(ctx, h, "go", JS_NewCFunction(ctx, js_history_traverse, "go", 1));
    return h;
}
