/* Document query methods — see document.h. Extracted from main.c. Real lookups over the live Lexbor DOM:
 * document.querySelector/All + getElementById + getElementsByClassName run the CSS selector engine and wrap the
 * result(s) as JS Elements, so a bundle walking the DOM to find a mount point / config node is explored. */
#include "core/dom/document.h"
#include "core/dom/dom_select.h"    /* dom_select_first / dom_select_all over the live Lexbor tree */
#include "core/dom/dom_element.h"   /* el_wrap: real Lexbor element -> JS Element */
#include "core/dom/custom_elements.h"   /* ce_upgrade — createElement upgrades a defined custom-element tag, else el_wrap */
#include "core/dom/node.h"           /* Document.prototype inherits Node.prototype (Document : Node : EventTarget) */
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

/* The Document INTERFACE — a real prototype-based class (Blink Document.idl), not a per-instance bag: the
   methods + computed getters live on Document.prototype (shared, so `document.querySelector ===
   Document.prototype.querySelector` and `document instanceof Document` hold), and window.Document is the
   interface object. Members implemented across sub-files (cookie.c/docwrite.c/domparser.c) but the INTERFACE is
   Document's — where a Blink engineer expects it. This is the faithful binding SHAPE the IDL audit drives toward,
   like cssom.c's CSSStyleDeclaration. Per-document snapshot VALUES (URL/body/title/forms/…) go on the instance. */
static JSClassID g_document_class_id;
static JSValue doc_illegal_ctor(JSContext *ctx, JSValueConst nt, int argc, JSValueConst *argv) {
    (void)nt; (void)argc; (void)argv;
    return JS_ThrowTypeError(ctx, "Illegal constructor");   /* Blink: `new Document()` from script throws */
}
static void doc_def_getset(JSContext *ctx, JSValue proto, const char *name, JSCFunction *get, JSCFunction *set) {
    JSAtom a = JS_NewAtom(ctx, name);
    JS_DefinePropertyGetSet(ctx, proto, a,
        JS_NewCFunction2(ctx, get, name, 0, JS_CFUNC_getter, 0),
        set ? JS_NewCFunction2(ctx, set, name, 1, JS_CFUNC_setter, 0) : JS_UNDEFINED,
        JS_PROP_CONFIGURABLE);
    JS_FreeAtom(ctx, a);
}
/* NAMED PROPERTIES (Blink WindowProperties / document named getter): a shipped element with an id is exposed as
   window[id] and document[id] — the REAL element, not an opaque {state} shrug (a named element is modelable, not
   injected app-state). So `window.myMount.appendChild(...)` / `if (window.cfg)` see the actual node. Installed as
   own props only where no real global/document member already exists (a real browser gives globals priority over
   named access). This is also the base for DOM-clobbering @S: an id-bearing element flips a global the app reads. */
void install_named_properties(JSContext *ctx, JSValue global, JSValue document) {
    JSValue els = dom_select_all(ctx, NULL, "[id]", 4);
    if (!JS_IsArray(els)) { JS_FreeValue(ctx, els); return; }
    uint32_t n = 0; { JSValue lv = JS_GetPropertyStr(ctx, els, "length"); JS_ToUint32(ctx, &n, lv); JS_FreeValue(ctx, lv); }
    for (uint32_t i = 0; i < n; i++) {
        JSValue el = JS_GetPropertyUint32(ctx, els, i);
        JSValue idv = JS_GetPropertyStr(ctx, el, "id");
        const char *id = JS_IsString(idv) ? JS_ToCString(ctx, idv) : NULL;
        if (id && id[0]) {
            JSAtom a = JS_NewAtom(ctx, id);
            if (!JS_HasProperty(ctx, global, a))   /* globals win over named access (spec) */
                JS_DefinePropertyValue(ctx, global, a, JS_DupValue(ctx, el), JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE);
            if (!JS_HasProperty(ctx, document, a))
                JS_DefinePropertyValue(ctx, document, a, JS_DupValue(ctx, el), JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE);
            JS_FreeAtom(ctx, a);
        }
        if (id) JS_FreeCString(ctx, id);
        JS_FreeValue(ctx, idv); JS_FreeValue(ctx, el);
    }
    JS_FreeValue(ctx, els);
}
void document_init(JSContext *ctx, JSValue global) {
    JSRuntime *rt = JS_GetRuntime(ctx);
    JS_NewClassID(rt, &g_document_class_id);
    JSClassDef def = { "Document" };
    JS_NewClass(rt, g_document_class_id, &def);
    JSValue proto = JS_NewObject(ctx);
    JS_SetPrototype(ctx, proto, node_proto(ctx));   /* Document IS a Node IS an EventTarget: inherit through the shared spine (Blink Document : Node : EventTarget) */
    /* METHODS on the shared prototype (Document.prototype). */
    JS_SetPropertyStr(ctx, proto, "querySelector", JS_NewCFunction(ctx, js_doc_querySelector, "querySelector", 1));
    JS_SetPropertyStr(ctx, proto, "querySelectorAll", JS_NewCFunction(ctx, js_doc_querySelectorAll, "querySelectorAll", 1));
    JS_SetPropertyStr(ctx, proto, "getElementById", JS_NewCFunction(ctx, js_doc_getElementById, "getElementById", 1));
    JS_SetPropertyStr(ctx, proto, "getElementsByTagName", JS_NewCFunction(ctx, js_doc_querySelectorAll, "getElementsByTagName", 1));
    JS_SetPropertyStr(ctx, proto, "getElementsByClassName", JS_NewCFunction(ctx, js_doc_getByClass, "getElementsByClassName", 1));
    JS_SetPropertyStr(ctx, proto, "createElement", JS_NewCFunction(ctx, js_doc_createElement, "createElement", 1));
    JS_SetPropertyStr(ctx, proto, "createRange", JS_NewCFunction(ctx, js_doc_createrange, "createRange", 0));   /* createContextualFragment -> {parsedhtml} taint */
    JS_SetPropertyStr(ctx, proto, "createTextNode", JS_NewCFunction(ctx, js_opaque_stub, "createTextNode", 1));
    JS_SetPropertyStr(ctx, proto, "createDocumentFragment", JS_NewCFunction(ctx, js_opaque_stub, "createDocumentFragment", 0));
    JS_SetPropertyStr(ctx, proto, "write", JS_NewCFunction(ctx, js_doc_write, "write", 1));
    JS_SetPropertyStr(ctx, proto, "writeln", JS_NewCFunction(ctx, js_doc_write, "writeln", 1));
    /* COMPUTED getters on the prototype (global-backed, correct for the singleton document). */
    doc_def_getset(ctx, proto, "cookie", (JSCFunction *)js_cookie_get, (JSCFunction *)js_cookie_set);   /* per-flow cookie jar (cookie.c) */
    doc_def_getset(ctx, proto, "location", (JSCFunction *)js_window_location_get, (JSCFunction *)js_window_location_set);   /* aliases window.location -> nav @S sink */
    doc_def_getset(ctx, proto, "currentScript", (JSCFunction *)js_doc_currentscript, NULL);   /* the executing inline script */
    def_source(ctx, proto, "referrer", 3);   /* attacker-influenced referring URL: forks + delivers the @S replay candidate */
    JS_SetClassProto(ctx, g_document_class_id, JS_DupValue(ctx, proto));   /* class instances inherit the prototype */
    /* window.Document — the interface object; Document.prototype = proto, so `document instanceof Document`. */
    JSValue ctor = JS_NewCFunction2(ctx, doc_illegal_ctor, "Document", 0, JS_CFUNC_constructor, 0);
    JS_SetConstructor(ctx, ctor, proto);   /* ctor.prototype = proto, proto.constructor = ctor */
    JS_SetPropertyStr(ctx, global, "Document", ctor);
    JS_FreeValue(ctx, proto);
}
/* The window.document INSTANCE: shares Document.prototype (methods/getters) and carries this document's snapshot
   VALUES (identity, title, the shipped body/head/forms/scripts). */
JSValue js_document_make(JSContext *ctx) {
    JSValue doc = JS_NewObjectClass(ctx, g_document_class_id);
    JS_SetPropertyStr(ctx, doc, "nodeType", JS_NewInt32(ctx, 9));   /* Node.DOCUMENT_NODE */
    JS_SetPropertyStr(ctx, doc, "nodeName", JS_NewString(ctx, "#document"));
    JS_SetPropertyStr(ctx, doc, "URL", JS_NewString(ctx, g_origin));           /* page identity: CONCRETE for URL building */
    JS_SetPropertyStr(ctx, doc, "domain", JS_NewString(ctx, location_host()));  /* page identity: CONCRETE (location.c) */
    JS_SetPropertyStr(ctx, doc, "documentElement", el_wrap(ctx, dom_select_first(NULL, "html", 4)));
    JS_SetPropertyStr(ctx, doc, "readyState", JS_NewString(ctx, "complete"));   /* boot ran -> a ready gate takes the ready arm */
    JS_SetPropertyStr(ctx, doc, "hidden", js_concolic(ctx, "{docHidden}", JS_FALSE));   /* page-visibility: concolic so `if(document.hidden)` forks (reaches the backgrounded-code arm), example false */
    JS_SetPropertyStr(ctx, doc, "visibilityState", js_concolic(ctx, "{visibilityState}", JS_NewString(ctx, "visible")));
    JS_SetPropertyStr(ctx, doc, "head", el_wrap(ctx, g_dom ? lxb_dom_interface_element(lxb_html_document_head_element(g_dom)) : NULL));
    JS_SetPropertyStr(ctx, doc, "body", el_wrap(ctx, g_dom ? lxb_dom_interface_element(lxb_html_document_body_element(g_dom)) : NULL));
    { lxb_dom_element_t *tt = dom_select_first(NULL, "title", 5);   /* document.title = the REAL <title> text */
      size_t tl = 0; lxb_char_t *txt = tt ? lxb_dom_node_text_content(lxb_dom_interface_node(tt), &tl) : NULL;
      JS_SetPropertyStr(ctx, doc, "title", JS_NewStringLen(ctx, txt ? (const char *)txt : "", txt ? tl : 0)); }
    JS_SetPropertyStr(ctx, doc, "forms", dom_select_all(ctx, NULL, "form", 4));
    JS_SetPropertyStr(ctx, doc, "scripts", dom_select_all(ctx, NULL, "script", 6));
    return doc;
}
