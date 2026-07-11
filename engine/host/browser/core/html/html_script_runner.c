/* HTML script runner — see html_script_runner.h. */
#include "core/html/html_script_runner.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <lexbor/html/html.h>
#include <lexbor/dom/dom.h>
#include "core/loader/document_scripts.h"   /* dom_collect_scripts, script_is_exec */
#include "core/loader/module_loader.h"      /* link_inline_module — an inline <script type=module> links via the map */
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
static int el_has_async(lxb_dom_element_t *el) {
    size_t vl = 0; return lxb_dom_element_get_attribute(el, (const lxb_char_t *)"async", 5, &vl) != NULL;   /* boolean attr (presence) — async external does NOT block document order */
}
/* Script-kind registry: the load kind recorded when each external is requested, read by qjs_provide. */
typedef struct { char *url; int kind; } SKind;
static SKind *g_skinds = NULL; static int g_skinds_n = 0, g_skinds_cap = 0;
/* Record the load kind at REQUEST time (boot cursor, a dynamically-inserted <script>, a dyn-import chunk) so
   qjs_provide routes module-vs-classic by the real script type — NOT by re-parsing the body (JS_DetectModule
   mis-detects a plain classic script as a module: it defaults is_module=true and only flips on an import error). */
void dom_script_kind_set(const char *url, int kind) {
    for (int i = 0; i < g_skinds_n; i++) if (strcmp(g_skinds[i].url, url) == 0) { g_skinds[i].kind = kind; return; }
    if (g_skinds_n >= g_skinds_cap) { int nc = g_skinds_cap ? g_skinds_cap * 2 : 8; SKind *n = realloc(g_skinds, (size_t)nc * sizeof(SKind)); if (!n) return; g_skinds = n; g_skinds_cap = nc; }
    g_skinds[g_skinds_n].url = strdup(url); g_skinds[g_skinds_n].kind = kind; g_skinds_n++;
}
int dom_script_kind(const char *url) { for (int i = 0; i < g_skinds_n; i++) if (strcmp(g_skinds[i].url, url) == 0) return g_skinds[i].kind; return SK_SYNC; }
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
            int kind = el_type_is_module(el) ? SK_MODULE : (el_has_async(el) ? SK_ASYNC : SK_SYNC);
            dom_script_kind_set(cu, kind);   /* record so qjs_provide routes it without re-parsing */
            if (kind != SK_SYNC) { arr_push_str(ctx, g_chunkurls, cu); if (!has_hole(cu)) chunk_pending_add(cu); free(cu); continue; }   /* async / module: does NOT block document order */
            size_t blen = 0; const char *body = has_hole(cu) ? NULL : modsrc_body(cu, &blen);
            if (body) { JSValue elw = el_wrap(ctx, el); run_classic_body(ctx, elw, body, blen); JS_FreeValue(ctx, elw); free(cu); continue; }
            arr_push_str(ctx, g_chunkurls, cu); if (!has_hole(cu)) chunk_pending_add(cu); free(cu);
            return 1;   /* SYNC external not yet fetched: request + PARK (blocks document order like a real browser) */
        }
        int is_mod; if (!script_is_exec(el, &is_mod)) continue;
        size_t tl = 0; lxb_char_t *txt = lxb_dom_node_text_content(lxb_dom_interface_node(el), &tl);
        if (txt && tl) {
            if (is_mod) { doc_set_current_script(ctx, el_wrap(ctx, el)); link_inline_module(ctx, (const char *)txt, tl); doc_set_current_script(ctx, JS_NULL); }   /* inline module -> the ONE URL-keyed map + tree-linker retry (like an external module) */
            else { JSValue elw = el_wrap(ctx, el); run_classic_body(ctx, elw, (const char *)txt, tl); JS_FreeValue(ctx, elw); }
        }
        if (txt) lxb_dom_document_destroy_text(lxb_dom_interface_node(el)->owner_document, txt);
    }
    return 0;   /* all scripts ran — boot script phase complete */
}
int dom_run_scripts(JSContext *ctx) {
    if (!g_dom) return 0;
    csp_derive(g_dom);   /* per-document effective CSP: real HTTP header (primary) else <meta> scan (csp.c) */
    for (int i = 0; i < g_skinds_n; i++) free(g_skinds[i].url);
    free(g_skinds); g_skinds = NULL; g_skinds_n = g_skinds_cap = 0;
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
