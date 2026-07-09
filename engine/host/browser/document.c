/* Document query methods — see document.h. Extracted from main.c. Real lookups over the live Lexbor DOM:
 * document.querySelector/All + getElementById + getElementsByClassName run the CSS selector engine and wrap the
 * result(s) as JS Elements, so a bundle walking the DOM to find a mount point / config node is explored. */
#include "document.h"
#include "dom_select.h"    /* dom_select_first / dom_select_all over the live Lexbor tree */
#include "dom_element.h"   /* el_wrap: real Lexbor element -> JS Element */

JSValue js_doc_querySelectorAll(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (argc < 1) return JS_NewArray(ctx);
    const char *s = JS_ToCString(ctx, argv[0]); if (!s) return JS_NewArray(ctx);
    JSValue r = dom_select_all(ctx, NULL, s, strlen(s)); JS_FreeCString(ctx, s); return r;
}
JSValue js_doc_getByClass(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (argc < 1) return JS_NewArray(ctx);
    const char *s = JS_ToCString(ctx, argv[0]); if (!s) return JS_NewArray(ctx);
    char sel[256]; snprintf(sel, sizeof sel, ".%s", s); JS_FreeCString(ctx, s);
    return dom_select_all(ctx, NULL, sel, strlen(sel));   /* getElementsByClassName -> .cls */
}
JSValue js_doc_getElementById(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (argc < 1) return JS_NULL;
    const char *id = JS_ToCString(ctx, argv[0]); if (!id) return JS_NULL;
    char sel[512]; snprintf(sel, sizeof sel, "#%s", id);
    lxb_dom_element_t *el = dom_select_first(NULL, sel, strlen(sel));
    JS_FreeCString(ctx, id);
    return el_wrap(ctx, el);
}
JSValue js_doc_querySelector(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (argc < 1) return JS_NULL;
    const char *s = JS_ToCString(ctx, argv[0]); if (!s) return JS_NULL;
    lxb_dom_element_t *el = dom_select_first(NULL, s, strlen(s));
    JS_FreeCString(ctx, s);
    return el_wrap(ctx, el);
}
