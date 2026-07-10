/* Document query methods — see document.h. Extracted from main.c. Real lookups over the live Lexbor DOM:
 * document.querySelector/All + getElementById + getElementsByClassName run the CSS selector engine and wrap the
 * result(s) as JS Elements, so a bundle walking the DOM to find a mount point / config node is explored. */
#include "core/dom/document.h"
#include "core/dom/dom_select.h"    /* dom_select_first / dom_select_all over the live Lexbor tree */
#include "core/dom/dom_element.h"   /* el_wrap: real Lexbor element -> JS Element */
#include "core/dom/custom_elements.h"   /* ce_upgrade — createElement upgrades a defined custom-element tag, else el_wrap */
#include "core/dom/domparser.h"     /* js_doc_createrange — document.createRange (contextual fragment taint) */
#include "core/html/docwrite.h"     /* js_doc_write — document.write/writeln */
#include "core/frame/cookie.h"      /* js_cookie_get/set — document.cookie per-flow jar */
#include "core/frame/location.h"    /* def_source (referrer), location_host (document.domain), window.location getset */
#include "solver/opaque.h"          /* js_opaque_stub — non-throwing DOM stubs (createTextNode/Fragment) */
#include "check.h"             /* DCHECK — the live document is an engine invariant (created at boot) */
#include <lexbor/html/html.h>  /* lxb_dom_document_create_element */
#include <string.h>

extern lxb_html_document_t *g_dom;   /* the live parsed document (main.c) */
extern const char *g_origin;         /* the page principal (main.c) — document.URL */
extern JSValue js_add_listener(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv);   /* scheduler edge: register a handler flow */

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

/* Build the window.document object — the Document interface's members (Blink Document.idl): identity (URL/domain
   /title/readyState), the per-flow cookie jar + attacker referrer source, the live DOM query methods, document.
   write, forms/scripts snapshots, and document.location aliasing window.location (so `document.location = url`
   stays a nav @S sink). Members implemented across sub-files (cookie.c/docwrite.c/domparser.c) but ASSEMBLED here
   because they are all Document members — where a Blink engineer expects the Document interface to own them. */
JSValue js_document_make(JSContext *ctx) {
    JSValue doc = JS_NewObject(ctx);
    { JSAtom ca = JS_NewAtom(ctx, "cookie");   /* document.cookie: a per-CODE-FLOW cookie jar (cookie.c) — round-trips concolic, not an opaque shrug */
      JS_DefinePropertyGetSet(ctx, doc, ca,
          JS_NewCFunction2(ctx, (JSCFunction *)js_cookie_get, "get cookie", 0, JS_CFUNC_getter, 0),
          JS_NewCFunction2(ctx, (JSCFunction *)js_cookie_set, "set cookie", 1, JS_CFUNC_setter, 0), JS_PROP_CONFIGURABLE);
      JS_FreeAtom(ctx, ca); }
    def_source(ctx, doc, "referrer", 3);   /* attacker-influenced referring URL: forks control flow + delivers the @S replay candidate */
    JS_SetPropertyStr(ctx, doc, "URL", JS_NewString(ctx, g_origin));           /* page identity: CONCRETE for URL building */
    JS_SetPropertyStr(ctx, doc, "domain", JS_NewString(ctx, location_host()));  /* page identity: CONCRETE (location.c) */
    JS_SetPropertyStr(ctx, doc, "addEventListener", JS_NewCFunction(ctx, js_add_listener, "addEventListener", 2));
    JS_SetPropertyStr(ctx, doc, "querySelector", JS_NewCFunction(ctx, js_doc_querySelector, "querySelector", 1));
    JS_SetPropertyStr(ctx, doc, "getElementById", JS_NewCFunction(ctx, js_doc_getElementById, "getElementById", 1));
    JS_SetPropertyStr(ctx, doc, "createElement", JS_NewCFunction(ctx, js_doc_createElement, "createElement", 1));
    JS_SetPropertyStr(ctx, doc, "createRange", JS_NewCFunction(ctx, js_doc_createrange, "createRange", 0));   /* createContextualFragment -> {parsedhtml} taint */
    JS_SetPropertyStr(ctx, doc, "createTextNode", JS_NewCFunction(ctx, js_opaque_stub, "createTextNode", 1));
    JS_SetPropertyStr(ctx, doc, "querySelectorAll", JS_NewCFunction(ctx, js_doc_querySelectorAll, "querySelectorAll", 1));
    JS_SetPropertyStr(ctx, doc, "getElementsByTagName", JS_NewCFunction(ctx, js_doc_querySelectorAll, "getElementsByTagName", 1));
    JS_SetPropertyStr(ctx, doc, "getElementsByClassName", JS_NewCFunction(ctx, js_doc_getByClass, "getElementsByClassName", 1));
    JS_SetPropertyStr(ctx, doc, "createDocumentFragment", JS_NewCFunction(ctx, js_opaque_stub, "createDocumentFragment", 0));
    JS_SetPropertyStr(ctx, doc, "write", JS_NewCFunction(ctx, js_doc_write, "write", 1));
    JS_SetPropertyStr(ctx, doc, "writeln", JS_NewCFunction(ctx, js_doc_write, "writeln", 1));
    JS_SetPropertyStr(ctx, doc, "head", el_wrap(ctx, g_dom ? lxb_dom_interface_element(lxb_html_document_head_element(g_dom)) : NULL));
    JS_SetPropertyStr(ctx, doc, "body", el_wrap(ctx, g_dom ? lxb_dom_interface_element(lxb_html_document_body_element(g_dom)) : NULL));
    JS_SetPropertyStr(ctx, doc, "documentElement", el_wrap(ctx, dom_select_first(NULL, "html", 4)));
    JS_SetPropertyStr(ctx, doc, "readyState", JS_NewString(ctx, "complete"));   /* boot ran -> a ready gate takes the ready arm */
    { lxb_dom_element_t *tt = dom_select_first(NULL, "title", 5);   /* document.title = the REAL <title> text (identity/config read) */
      size_t tl = 0; lxb_char_t *txt = tt ? lxb_dom_node_text_content(lxb_dom_interface_node(tt), &tl) : NULL;
      JS_SetPropertyStr(ctx, doc, "title", JS_NewStringLen(ctx, txt ? (const char *)txt : "", txt ? tl : 0)); }
    { JSAtom a = JS_NewAtom(ctx, "location");   /* document.location aliases window.location (getset -> nav @S sink) */
      JS_DefinePropertyGetSet(ctx, doc, a, JS_NewCFunction2(ctx, (JSCFunction *)js_window_location_get, "get", 0, JS_CFUNC_getter, 0),
          JS_NewCFunction2(ctx, (JSCFunction *)js_window_location_set, "set", 1, JS_CFUNC_setter, 0), JS_PROP_CONFIGURABLE);
      JS_FreeAtom(ctx, a); }
    JS_SetPropertyStr(ctx, doc, "forms", dom_select_all(ctx, NULL, "form", 4));
    JS_SetPropertyStr(ctx, doc, "scripts", dom_select_all(ctx, NULL, "script", 6));
    { JSAtom a = JS_NewAtom(ctx, "currentScript");   /* getter: the executing script, changes per inline script */
      JS_DefinePropertyGetSet(ctx, doc, a, JS_NewCFunction2(ctx, (JSCFunction *)js_doc_currentscript, "get", 0, JS_CFUNC_getter, 0), JS_UNDEFINED, JS_PROP_CONFIGURABLE);
      JS_FreeAtom(ctx, a); }
    return doc;
}
