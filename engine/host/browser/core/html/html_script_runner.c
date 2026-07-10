/* HTML script runner — see html_script_runner.h. */
#include "core/html/html_script_runner.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <lexbor/html/html.h>
#include <lexbor/dom/dom.h>
#include "core/loader/document_scripts.h"   /* dom_collect_scripts, script_is_exec */
#include "core/loader/module_loader.h"      /* module_next_name, pendmod_add — ESM eval + deferral */
#include "core/frame/csp.h"                 /* csp_derive — per-document effective CSP */
#include "core/dom/document.h"              /* doc_set_current_script — document.currentScript */
#include "core/dom/dom_element.h"           /* el_wrap — the running <script> element */
#include "platform/url.h"                   /* has_hole — an opaque-hole src isn't a concrete fetch */
#include "solver/boot_scripts.h"             /* boot_script_cache — inline scripts cached for boot-replay */
#include "solver/why.h"                      /* why_add — a script runtime throw surfaces as @WHY */

/* main.c host edges (the scheduler's resource loader + the live document) — externed until the loader itself is
   a component; a browser component uses them by interface, not by owning the state. */
extern lxb_html_document_t *g_dom;
extern JSValue g_chunkurls;                              /* discovered external <script src> URLs */
extern void arr_push_str(JSContext *ctx, JSValueConst arr, const char *s);
extern void chunk_pending_add(const char *url);          /* queue an external script for host fetch + eval */

/* Run a page script LIKE A BROWSER. is_module is the REAL browser signal — the <script type="module"> attribute
   for inline, JS_DetectModule(body) for a fetched chunk. A classic script runs GLOBAL and its runtime throw
   surfaces as @WHY (never swallowed); a module runs as ESM and, if a static-import dep isn't fetched yet, defers
   into the module loader (retried on each qjs_provide). */
void eval_page_script(JSContext *ctx, const char *code, size_t len, const char *name, int is_module) {
    if (is_module) {
        char nm[32]; module_next_name(nm, sizeof nm);    /* unique <mod-N> so no name collision on defer/retry */
        JSValue v = JS_Eval(ctx, code, len, nm, JS_EVAL_TYPE_MODULE);
        if (JS_IsException(v)) { JS_FreeValue(ctx, JS_GetException(ctx)); pendmod_add(code, len); }  /* dep not fetched yet: defer */
        else { JSContext *c; while (JS_ExecutePendingJob(JS_GetRuntime(ctx), &c) > 0) {} }  /* module eval is async -> drive it */
        JS_FreeValue(ctx, v);
        return;
    }
    JSValue v = JS_Eval(ctx, code, len, name, JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(v)) {   /* a genuine RUNTIME throw -> surface it, never silently swallow */
        JSValue e = JS_GetException(ctx); const char *m = JS_ToCString(ctx, e);
        char rz[300]; snprintf(rz, sizeof rz, "%s: %s", name ? name : "?", m ? m : "throw");
        why_add(ctx, "script-eval", rz);
        if (m) JS_FreeCString(ctx, m); JS_FreeValue(ctx, e);
    }
    JS_FreeValue(ctx, v);
}

void dom_run_scripts(JSContext *ctx) {
    if (!g_dom) return;
    csp_derive(g_dom);   /* per-document effective CSP: real HTTP header (primary) else <meta> scan (csp.c) */
    struct scr_ctx c; dom_collect_scripts(g_dom, &c);
    for (int i = 0; i < c.n; i++) {
        lxb_dom_element_t *el = c.els[i];
        size_t sl = 0;
        const lxb_char_t *src = lxb_dom_element_get_attribute(el, (const lxb_char_t *)"src", 3, &sl);
        if (src && sl) {
            char *cu = strndup((const char *)src, sl);
            /* LOAD it like a real browser: an external <script src> is FETCHED (safe-fetch chokepoint,
               cross-origin allowed) and RUN through the engine. chunk_pending_add -> host NEED_FETCH ->
               qjs_provide evals + caches it, so the bundle's endpoints/handlers/cross-flow are analyzed. */
            if (cu) { arr_push_str(ctx, g_chunkurls, cu); if (!has_hole(cu)) chunk_pending_add(cu); free(cu); }
            continue;
        }
        int is_mod; if (!script_is_exec(el, &is_mod)) continue;   /* data block: parsed-but-not-run */
        size_t tl = 0; lxb_char_t *txt = lxb_dom_node_text_content(lxb_dom_interface_node(el), &tl);
        if (txt && tl) {
            boot_script_cache((const char *)txt, tl);   /* cache for cross-flow @S candidate boot-replay */
            doc_set_current_script(ctx, el_wrap(ctx, el));   /* document.currentScript during this inline script */
            eval_page_script(ctx, (const char *)txt, tl, "<script>", is_mod);
            doc_set_current_script(ctx, JS_NULL);
        }
        if (txt) lxb_dom_document_destroy_text(lxb_dom_interface_node(el)->owner_document, txt);
    }
    free(c.els);
}
