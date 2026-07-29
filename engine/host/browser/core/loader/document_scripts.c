/* Document script inventory + bundle identity — see document_scripts.h. */
#include "core/loader/document_scripts.h"
#include <string.h>
#include <stdlib.h>
#include <lexbor/css/css.h>
#include <lexbor/selectors/selectors.h>

static lxb_status_t scr_collect_cb(lxb_dom_node_t *node, lxb_css_selector_specificity_t s, void *vp) {
    struct scr_ctx *c = vp; (void)s;
    if (c->n >= c->cap) { int nc = c->cap ? c->cap * 2 : 8;
        lxb_dom_element_t **n = realloc(c->els, (size_t)nc * sizeof(lxb_dom_element_t *)); if (!n) return LXB_STATUS_OK; c->els = n; c->cap = nc; }
    c->els[c->n++] = lxb_dom_interface_element(node);
    return LXB_STATUS_OK;
}

void dom_collect_scripts(lxb_html_document_t *dom, struct scr_ctx *out) {
    out->els = NULL; out->n = 0; out->cap = 0;
    if (!dom) return;
    lxb_css_parser_t *p = lxb_css_parser_create();
    if (!p || lxb_css_parser_init(p, NULL) != LXB_STATUS_OK) { if (p) lxb_css_parser_destroy(p, true); return; }
    lxb_css_selector_list_t *list = lxb_css_selectors_parse(p, (const lxb_char_t *)"script", 6);
    if (!list) { lxb_css_parser_destroy(p, true); return; }
    lxb_selectors_t *sel = lxb_selectors_create();
    if (!sel || lxb_selectors_init(sel) != LXB_STATUS_OK) { if (sel) lxb_selectors_destroy(sel, true); lxb_css_selector_list_destroy_memory(list); lxb_css_parser_destroy(p, true); return; }
    lxb_selectors_find(sel, lxb_dom_interface_node(dom), list, scr_collect_cb, out);
    /* the selector list owns its OWN css memory arena (lxb_css_selectors_parse allocates it), separate from the
       parser's — destroying the parser does NOT free it, so free it explicitly or it leaks per call. */
    lxb_selectors_destroy(sel, true); lxb_css_selector_list_destroy_memory(list); lxb_css_parser_destroy(p, true);
}

int script_is_exec(lxb_dom_element_t *el, int *is_mod) {
    *is_mod = 0;
    size_t tyl = 0; const lxb_char_t *ty = lxb_dom_element_get_attribute(el, (const lxb_char_t *)"type", 4, &tyl);
    if (!ty || !tyl) return 1;   /* no type -> classic executable script */
    char tb[64]; size_t tn = tyl < 63 ? tyl : 63;
    for (size_t k = 0; k < tn; k++) { char ch = (char)ty[k]; tb[k] = (ch >= 'A' && ch <= 'Z') ? (char)(ch + 32) : ch; }
    tb[tn] = 0;
    if (strcmp(tb, "module") == 0) { *is_mod = 1; return 1; }
    return (strstr(tb, "javascript") || strstr(tb, "ecmascript")) ? 1 : 0;   /* JS MIME -> exec; else data */
}
/* A <script type="importmap"> data block — parsed (not executed) into the bare-specifier resolver. */
int script_is_importmap(lxb_dom_element_t *el) {
    size_t tyl = 0; const lxb_char_t *ty = lxb_dom_element_get_attribute(el, (const lxb_char_t *)"type", 4, &tyl);
    if (!ty || tyl != 9) return 0;
    char tb[10]; for (size_t k = 0; k < 9; k++) { char c = (char)ty[k]; tb[k] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; } tb[9] = 0;
    return strcmp(tb, "importmap") == 0;
}

unsigned document_bundle_id(lxb_html_document_t *dom) {
    struct scr_ctx c; dom_collect_scripts(dom, &c);
    uint32_t bh = 2166136261u;
    for (int i = 0; i < c.n; i++) {
        lxb_dom_element_t *el = c.els[i];
        size_t sl = 0;
        const lxb_char_t *src = lxb_dom_element_get_attribute(el, (const lxb_char_t *)"src", 3, &sl);
        if (src && sl) {
            for (size_t k = 0; k < sl; k++) { bh ^= src[k]; bh *= 16777619u; }   /* external src URL -> bundle id */
            bh ^= '|'; bh *= 16777619u;
            continue;
        }
        int is_mod; if (!script_is_exec(el, &is_mod)) continue;   /* data block: not part of JS identity */
        size_t tl = 0; lxb_char_t *txt = lxb_dom_node_text_content(lxb_dom_interface_node(el), &tl);
        if (txt && tl) { for (size_t k = 0; k < tl; k++) { bh ^= txt[k]; bh *= 16777619u; } bh ^= '|'; bh *= 16777619u; }
        if (txt) lxb_dom_document_destroy_text(lxb_dom_interface_node(el)->owner_document, txt);
    }
    free(c.els);
    return bh ? bh : 1;
}

DocScripts document_exec_scripts(lxb_html_document_t *dom) {
    struct scr_ctx c; dom_collect_scripts(dom, &c);
    DocScripts ds = { NULL, 0 };
    if (c.n) { ds.bodies = calloc((size_t)c.n, sizeof(char *)); if (!ds.bodies) { free(c.els); return ds; } }
    /* Each OWN inline executable script becomes its OWN body (document order) — never concatenated, so its
       top-level let/const stays script-scoped. An external `src` script is a fetch-then-run flow (later); a
       data block (json/importmap) is parsed, never run. */
    for (int i = 0; i < c.n; i++) {
        lxb_dom_element_t *el = c.els[i];
        size_t sl = 0;
        const lxb_char_t *src = lxb_dom_element_get_attribute(el, (const lxb_char_t *)"src", 3, &sl);
        if (src && sl) continue;
        int is_mod; if (!script_is_exec(el, &is_mod)) continue;
        size_t tl = 0; lxb_char_t *txt = lxb_dom_node_text_content(lxb_dom_interface_node(el), &tl);
        if (txt && tl && tl < SIZE_MAX) {
            char *b = malloc(tl + 1);
            if (b) { memcpy(b, txt, tl); b[tl] = 0; ds.bodies[ds.n++] = b; }
        }
        if (txt) lxb_dom_document_destroy_text(lxb_dom_interface_node(el)->owner_document, txt);
    }
    free(c.els);
    return ds;
}

void doc_scripts_free(DocScripts *ds) {
    if (!ds || !ds->bodies) return;
    for (int i = 0; i < ds->n; i++) free(ds->bodies[i]);
    free(ds->bodies); ds->bodies = NULL; ds->n = 0;
}
