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

/* The ONE classic-boot-script executor (see header): currentScript + run the parse-once bytecode, shared by the
   first boot and every replay. Returns 1 if it threw (exception left PENDING for the caller's throw policy). */
int boot_exec_one(JSContext *ctx, JSValueConst el, JSValueConst compiled) {
    if (JS_IsException(compiled)) return 1;                       /* a syntax error from boot_script_cache: exception already pending */
    doc_set_current_script(ctx, JS_DupValue(ctx, el));           /* document.currentScript = the <script> element (or JS_NULL) */
    JSValue v = JS_EvalFunction(ctx, JS_DupValue(ctx, compiled)); /* run the bytecode (dup: the cache keeps it for every replay) */
    int threw = JS_IsException(v);                               /* leaves the exception PENDING for JS_GetException */
    doc_set_current_script(ctx, JS_NULL);
    JS_FreeValue(ctx, v);
    return threw;
}

#include "core/loader/module_loader.h"   /* modsrc_body — a fetched sync external's body */
static void run_classic_body(JSContext *ctx, JSValueConst el, const char *txt, size_t len) {
    JSValueConst compiled = boot_script_cache(ctx, el, txt, len);
    if (boot_exec_one(ctx, el, compiled)) {
        JSValue e = JS_GetException(ctx); const char *m = JS_ToCString(ctx, e);
        char rz[300]; snprintf(rz, sizeof rz, "<script>: %s", m ? m : "throw");
        if (m) JS_FreeCString(ctx, m); JS_FreeValue(ctx, e);
        why_add(ctx, "script-eval", rz);
    }
}
static int el_type_is_module(lxb_dom_element_t *el) {
    size_t tl = 0; const lxb_char_t *t = lxb_dom_element_get_attribute(el, (const lxb_char_t *)"type", 4, &tl);
    return t && tl == 6 && memcmp(t, "module", 6) == 0;
}
/* Document-order boot cursor: run <script>s in order, BLOCKING (park) on an unfetched synchronous external. */
static struct scr_ctx g_boot_scr = {0};
static int g_boot_cursor = 0;
static int boot_drive_scripts(JSContext *ctx) {
    for (; g_boot_cursor < g_boot_scr.n; g_boot_cursor++) {
        lxb_dom_element_t *el = g_boot_scr.els[g_boot_cursor];
        size_t sl = 0;
        const lxb_char_t *src = lxb_dom_element_get_attribute(el, (const lxb_char_t *)"src", 3, &sl);
        if (src && sl) {
            char *cu = strndup((const char *)src, sl); if (!cu) continue;
            if (el_type_is_module(el)) { arr_push_str(ctx, g_chunkurls, cu); if (!has_hole(cu)) chunk_pending_add(cu); free(cu); continue; }   /* external module: async */
            size_t blen = 0; const char *body = has_hole(cu) ? NULL : modsrc_body(cu, &blen);
            if (body) { JSValue elw = el_wrap(ctx, el); run_classic_body(ctx, elw, body, blen); JS_FreeValue(ctx, elw); free(cu); continue; }
            arr_push_str(ctx, g_chunkurls, cu); if (!has_hole(cu)) chunk_pending_add(cu); free(cu);
            return 1;   /* request + PARK (blocks document order until this sync external is fetched) */
        }
        int is_mod; if (!script_is_exec(el, &is_mod)) continue;
        size_t tl = 0; lxb_char_t *txt = lxb_dom_node_text_content(lxb_dom_interface_node(el), &tl);
        if (txt && tl) {
            if (is_mod) { doc_set_current_script(ctx, el_wrap(ctx, el)); eval_page_script(ctx, (const char *)txt, tl, "<script>", 1); doc_set_current_script(ctx, JS_NULL); }
            else { JSValue elw = el_wrap(ctx, el); run_classic_body(ctx, elw, (const char *)txt, tl); JS_FreeValue(ctx, elw); }
        }
        if (txt) lxb_dom_document_destroy_text(lxb_dom_interface_node(el)->owner_document, txt);
    }
    return 0;   /* all scripts ran — boot script phase complete */
}
int dom_run_scripts(JSContext *ctx) {
    if (!g_dom) return 0;
    csp_derive(g_dom);   /* per-document effective CSP: real HTTP header (primary) else <meta> scan (csp.c) */
    free(g_boot_scr.els); g_boot_scr.els = NULL; g_boot_scr.n = 0;
    dom_collect_scripts(g_dom, &g_boot_scr);
    g_boot_cursor = 0;
    return boot_drive_scripts(ctx);
}
int dom_boot_resume(JSContext *ctx) { return boot_drive_scripts(ctx); }
/* Is `url` the synchronous CLASSIC external the boot cursor is currently PARKED on? (Lets qjs_provide tell a
   boot-blocking external — resume the cursor — from a module chunk a boot script dynamically imported.) */
int dom_boot_parked_is(const char *url) {
    if (g_boot_cursor >= g_boot_scr.n) return 0;
    size_t sl = 0;
    const lxb_char_t *src = lxb_dom_element_get_attribute(g_boot_scr.els[g_boot_cursor], (const lxb_char_t *)"src", 3, &sl);
    return src && url && sl == strlen(url) && memcmp(src, url, sl) == 0;
}
