/* document.write() — @S sink + script loader. See docwrite.h. */
#include <string.h>
#include <stdlib.h>
#include "docwrite.h"
#include "solve.h"   /* solve_add — the written HTML is an XSS sink */
#include "url.h"     /* has_hole — a concrete (non-hole) chunk src is fetchable */
#include <lexbor/html/html.h>
#include <lexbor/dom/dom.h>

/* Borrowed from main.c: the chunk-load registrar (host NEED_FETCH -> qjs_provide), the discovered-chunk-URL
   array, and the string-array push helper. */
extern void chunk_pending_add(const char *url);
extern JSValue g_chunkurls;
extern void arr_push_str(JSContext *ctx, JSValueConst arr, const char *s);

/* Run one written <script>: an external src is a chunk load; an inline body is eval'd top-level. A written
   script that throws is caught; pathological document.write-of-a-script recursion is a loud crash TODO, never
   a re-entrancy-guard workaround. */
static lxb_status_t dw_script_run_cb(lxb_dom_node_t *node, void *vctx) {
    JSContext *ctx = vctx;
    if (node->type != LXB_DOM_NODE_TYPE_ELEMENT) return LXB_STATUS_OK;
    lxb_dom_element_t *el = lxb_dom_interface_element(node);
    size_t nl = 0; const lxb_char_t *nm = lxb_dom_element_qualified_name(el, &nl);
    if (!(nl == 6 && nm && memcmp(nm, "script", 6) == 0)) return LXB_STATUS_OK;
    size_t sl = 0;
    const lxb_char_t *src = lxb_dom_element_get_attribute(el, (const lxb_char_t *)"src", 3, &sl);
    if (src && sl) {   /* external <script src>: a chunk LOAD (document.write('<script src=...>')) — fetch + run it, not just string-extract */
        char *u = strndup((const char *)src, sl);
        if (u) { arr_push_str(ctx, g_chunkurls, u); if (!has_hole(u)) chunk_pending_add(u); free(u); }
        return LXB_STATUS_OK;
    }
    size_t tl = 0; lxb_char_t *txt = lxb_dom_node_text_content(lxb_dom_interface_node(el), &tl);
    if (!txt || tl == 0) return LXB_STATUS_OK;
    char *code = strndup((const char *)txt, tl);   /* null-terminate for JS_Eval */
    if (!code) return LXB_STATUS_OK;
    JSValue r = JS_Eval(ctx, code, tl, "<docwrite>", JS_EVAL_TYPE_GLOBAL);   /* TOP-LEVEL, global scope */
    if (JS_IsException(r)) { JSValue e = JS_GetException(ctx); JS_FreeValue(ctx, e); }
    JS_FreeValue(ctx, r);
    free(code);
    return LXB_STATUS_OK;
}
static void doc_write_run_scripts(JSContext *ctx, const char *html) {
    lxb_html_document_t *doc = lxb_html_document_create();
    if (!doc) return;
    if (lxb_html_document_parse(doc, (const lxb_char_t *)html, strlen(html)) == LXB_STATUS_OK) {
        /* Walk the WHOLE document, not just <body>: a bare `<script src>` (the common document.write form)
           parses into <head>, so a body-only walk never reached it — the external-script load silently no-op'd. */
        lxb_dom_node_t *root = lxb_dom_interface_node(doc);
        if (root) lxb_dom_node_simple_walk(root, dw_script_run_cb, ctx);
    }
    lxb_html_document_destroy(doc);
}
JSValue js_doc_write(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    for (int i = 0; i < argc; i++) {
        solve_add(ctx, "document.write", "htmls", argv[i]);   /* @S: written HTML is attacker-influenced -> XSS check */
        /* CONCRETE or CONCOLIC written HTML: run its inline scripts + load its <script src> chunks. A concolic
           value (a reply/computed HTML like '<script src="'+cfg.url+'">') is an opaque OBJECT, not a JS string,
           so JS_IsString alone skipped it — dropping the real example. Prefer the concolic example (concrete
           src), exactly like fetch/import/script.src. */
        JSValue ex = JS_IsString(argv[i]) ? JS_UNDEFINED : JS_OpaqueExample(ctx, argv[i]);
        JSValueConst hv = !JS_IsUndefined(ex) ? ex : argv[i];
        if (JS_IsString(hv)) {
            const char *html = JS_ToCString(ctx, hv);
            if (html && strstr(html, "<script")) doc_write_run_scripts(ctx, html);
            if (html) JS_FreeCString(ctx, html);
        }
        JS_FreeValue(ctx, ex);
    }
    return JS_UNDEFINED;
}
