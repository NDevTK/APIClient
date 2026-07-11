/* DOMParser + Range HTML parsing — see domparser.h. */
#include "core/dom/domparser.h"
#include "solver/opaque.h"   /* g_opaque, js_noop */

/* Borrowed from main.c (the @S candidate side): a replay flow pins the concrete candidate here, so a parsed
   string carries it as the example (else the parsed node is a bare {parsedhtml} shape). */
extern char *g_candidate;

/* Parse an HTML string into a {parsedhtml} OPAQUE carrying the input's concolic example — the taint a later
   appendChild/innerHTML of the node detects as an @S sink. */
static JSValue js_parse_html_tainted(JSContext *ctx, JSValueConst input) {
    JSValue r = JS_NewConcolicSourced(ctx, "{parsedhtml}", "{parsedhtml}");
    if (JS_IsConcolic(r)) {
        JSValue ex = JS_UNDEFINED;
        if (JS_IsConcolic(input)) ex = JS_ConcolicExample(ctx, input);                    /* concolic input keeps its example */
        else if (g_candidate && JS_IsString(input)) ex = JS_DupValue(ctx, input);     /* replay: the concrete candidate */
        if (!JS_IsUndefined(ex)) JS_SetConcolicExample(ctx, r, ex); else JS_FreeValue(ctx, ex);
    }
    return r;
}
static JSValue js_domparser_parse(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    return argc >= 1 ? js_parse_html_tainted(ctx, argv[0]) : js_concolic(ctx, "{parsedhtml}", JS_UNDEFINED);
}
JSValue js_domparser_ctor(JSContext *ctx, JSValueConst nt, int argc, JSValueConst *argv) {
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "parseFromString", JS_NewCFunction(ctx, js_domparser_parse, "parseFromString", 2));
    return o;
}
static JSValue js_range_ccf(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    return argc >= 1 ? js_parse_html_tainted(ctx, argv[0]) : js_concolic(ctx, "{parsedhtml}", JS_UNDEFINED);
}
JSValue js_doc_createrange(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "createContextualFragment", JS_NewCFunction(ctx, js_range_ccf, "createContextualFragment", 1));
    const char *noops[] = { "selectNode", "selectNodeContents", "setStart", "setEnd", "deleteContents", "insertNode", "surroundContents", "cloneContents" };
    for (size_t i = 0; i < sizeof noops / sizeof noops[0]; i++)
        JS_SetPropertyStr(ctx, o, noops[i], JS_NewCFunction(ctx, js_noop, noops[i], 1));
    return o;
}
