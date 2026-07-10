/* Document query methods — see document.h. Extracted from main.c. Real lookups over the live Lexbor DOM:
 * document.querySelector/All + getElementById + getElementsByClassName run the CSS selector engine and wrap the
 * result(s) as JS Elements, so a bundle walking the DOM to find a mount point / config node is explored. */
#include "core/dom/document.h"
#include "core/dom/dom_select.h"    /* dom_select_first / dom_select_all over the live Lexbor tree */
#include "core/dom/dom_element.h"   /* el_wrap: real Lexbor element -> JS Element */
#include "core/dom/custom_elements.h"   /* ce_upgrade — createElement upgrades a defined custom-element tag, else el_wrap */
#include "check.h"             /* DCHECK — the live document is an engine invariant (created at boot) */
#include <lexbor/html/html.h>  /* lxb_dom_document_create_element */
#include <string.h>

extern lxb_html_document_t *g_dom;   /* the live parsed document (main.c) */

/* document.createElement(tag): a REAL Lexbor element (its methods/attrs work); a defined custom-element tag is
   upgraded (connectedCallback driven). appendChild of a returned <script src> is intercepted downstream. */
JSValue js_doc_createElement(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    DCHECK(g_dom, "createElement: g_dom NULL — the live document is created at boot before any script runs, so this is impossible");
    if (argc < 1) return JS_NULL;   /* createElement() with no arg: lenient null (a real browser throws TypeError; the page's bug, not ours) */
    const char *tag = JS_ToCString(ctx, argv[0]); if (!tag) return JS_NULL;
    lxb_dom_element_t *el = lxb_dom_document_create_element(lxb_dom_interface_document(g_dom), (const lxb_char_t *)tag, strlen(tag), NULL);
    JSValue r = ce_upgrade(ctx, el, tag);   /* Blink custom-element upgrade (custom_elements.c), else el_wrap */
    JS_FreeCString(ctx, tag);
    return r;
}

/* document.currentScript: the executing <script> during boot — a config-injection source (an embed reads
   document.currentScript.dataset.apiKey). The scheduler's boot loop FEEDS it per inline script via
   doc_set_current_script; the getter reads it. Document owns the state; the scheduler only drives it. */
static JSValue g_current_script = JS_NULL;
void doc_set_current_script(JSContext *ctx, JSValue v) { JS_FreeValue(ctx, g_current_script); g_current_script = v; }
JSValue js_doc_currentscript(JSContext *ctx, JSValueConst t) { (void)t; return JS_DupValue(ctx, g_current_script); }

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
