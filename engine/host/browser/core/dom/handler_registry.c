/* EVENT-HANDLER REGISTRY — see handler_registry.h. The page's addEventListener listeners, kept so the
   forced-exec orphan driver can fire the never-firing ones (the unused gated surface). */
#include <stdlib.h>
#include <string.h>
#include "core/dom/handler_registry.h"
#include "core/html/html_script_element.h"   /* script_load_bind_if — a <script>'s 'load' handler is load-gated (fires on chunk-provide, not seed) */
#include "solver/scheduler.h"   /* g_in_boot_flow / g_boot_replay: when a boot flow / candidate replay is running, don't grow the registry */

JSValue g_handlers = JS_UNDEFINED;
int g_handler_n = 0;
/* Handlers registered for 'message' (postMessage). Driven with a synthetic MessageEvent whose .data is
   the source-tagged opaque {pm}, so a postMessage-XSS sink reports {pm} and the PoC assembler builds a
   postMessage-delivered PoC. Borrowed refs (also held live in g_handlers). */
void *g_msg_handlers[128];
int g_msg_handler_n = 0;
/* Handlers RE-REGISTERED during a candidate boot_replay (addEventListener) — captured so a closure handler
   (which isn't on any global) can be re-resolved to its candidate-closure version. Transient per candidate
   flow; cleared after the drive. */
JSValue *g_replay_handlers = NULL; int g_replay_handler_n = 0, g_replay_handler_cap = 0;
void *g_replay_msg[128]; int g_replay_msg_n = 0;   /* re-registered 'message' handler ptrs (candidate closure) */
void replay_handlers_clear(JSContext *ctx) {
    for (int i = 0; i < g_replay_handler_n; i++) JS_FreeValue(ctx, g_replay_handlers[i]);
    g_replay_handler_n = 0; g_replay_msg_n = 0;
}
/* Is `fn` a registered event handler (addEventListener)? Then orphan-driving it must pass a real Event (not a
   bare opaque), else `if(e.preventDefault)`-style shape checks FORK an impossible arm -> a phantom endpoint. */
int is_handler(JSContext *ctx, JSValueConst fn) {
    if (JS_IsUndefined(g_handlers) || !JS_IsFunction(ctx, fn)) return 0;
    void *p = JS_VALUE_GET_PTR(fn);
    for (int i = 0; i < g_handler_n; i++) {
        JSValue h = JS_GetPropertyUint32(ctx, g_handlers, (uint32_t)i);
        int m = (JS_VALUE_GET_PTR(h) == p); JS_FreeValue(ctx, h);
        if (m) return 1;
    }
    return 0;
}
JSValue js_add_listener(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    JSValueConst h0 = (argc >= 2) ? argv[1] : (argc >= 1 ? argv[0] : JS_UNDEFINED);
    if (g_in_boot_flow && !g_initial_boot) return JS_UNDEFINED;   /* a boot flow RE-RUN re-encounters the same listeners -> don't duplicate; the INITIAL boot (g_initial_boot=1) is where the page's listeners FIRST register (run_initial_boot marks the first boot a boot flow, so the bare g_in_boot_flow guard wrongly dropped ALL of them -> g_handlers empty, breaking the @S session flow / message-driving / is_handler) */
    if (g_boot_replay) {   /* capture the re-registered handler (candidate closure) for re-resolution; don't grow g_handlers */
        if (JS_IsFunction(ctx, h0)) {
            if (g_replay_handler_n >= g_replay_handler_cap) { int nc = g_replay_handler_cap ? g_replay_handler_cap * 2 : 16;
                JSValue *n = realloc(g_replay_handlers, (size_t)nc * sizeof(JSValue)); if (n) { g_replay_handlers = n; g_replay_handler_cap = nc; } }
            if (g_replay_handler_n < g_replay_handler_cap) g_replay_handlers[g_replay_handler_n++] = JS_DupValue(ctx, h0);
            const char *type = argc >= 2 ? JS_ToCString(ctx, argv[0]) : NULL;   /* a re-registered 'message' handler must still be driven with the {pm} event */
            if (type && strcmp(type, "message") == 0 && g_replay_msg_n < 128) g_replay_msg[g_replay_msg_n++] = JS_VALUE_GET_PTR(h0);
            if (type) JS_FreeCString(ctx, type);
        }
        return JS_UNDEFINED;
    }
    JSValueConst h = (argc >= 2) ? argv[1] : (argc >= 1 ? argv[0] : JS_UNDEFINED);
    if (JS_IsFunction(ctx, h) && !JS_IsUndefined(g_handlers)) {
        JS_SetPropertyUint32(ctx, g_handlers, (uint32_t)g_handler_n++, JS_DupValue(ctx, h));
        const char *type = argc >= 2 ? JS_ToCString(ctx, argv[0]) : NULL;   /* addEventListener(type, handler) */
        if (type && strcmp(type, "message") == 0 && g_msg_handler_n < 128)
            g_msg_handlers[g_msg_handler_n++] = JS_VALUE_GET_PTR(h);
        script_load_bind_if(ctx, t, type, h);   /* a <script>'s 'load' handler is load-gated: drive it on chunk-provide, not boot seed */
        if (type) JS_FreeCString(ctx, type);
    }
    return JS_UNDEFINED;
}
int is_msg_handler(JSValueConst h) {
    void *p = JS_VALUE_GET_PTR(h);
    for (int i = 0; i < g_msg_handler_n; i++) if (g_msg_handlers[i] == p) return 1;
    for (int i = 0; i < g_replay_msg_n; i++) if (g_replay_msg[i] == p) return 1;   /* re-resolved candidate 'message' closure */
    return 0;
}

/* The registry's lifecycle — called from the engine's qjs_init / qjs_teardown. */
void handlers_init(JSContext *ctx) { g_handlers = JS_NewArray(ctx); }
void handlers_free(JSContext *ctx) {
    replay_handlers_clear(ctx); free(g_replay_handlers); g_replay_handlers = NULL; g_replay_handler_cap = 0;
    JS_FreeValue(ctx, g_handlers); g_handlers = JS_UNDEFINED; g_handler_n = 0; g_msg_handler_n = 0;
}
