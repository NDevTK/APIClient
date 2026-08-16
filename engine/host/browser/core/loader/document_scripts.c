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

/* Infra's ASCII whitespace: TAB, LF, FF, CR, SPACE — the set §4.12.1 strips from the type attribute. */
static int scr_ascii_ws(lxb_char_t c) {
    return c == 0x09 || c == 0x0A || c == 0x0C || c == 0x0D || c == 0x20;
}

/* HTML §4.12.1 "prepare the script element", the type-string steps. THE ANSWER IS THE RETURN VALUE: this was
   `script_is_exec(el, &is_mod)`, which computed the module bit into an out-parameter that BOTH of its callers
   declared and never read — so a module script became a classic one at the compile and its top-level `await`
   was reported as a SyntaxError. `script_is_importmap` was a second, narrower parser of the same attribute and
   had no caller at all; both are gone, replaced by the one question the spec asks once. */
ScriptType script_block_type(lxb_dom_element_t *el) {
    size_t tyl = 0;
    const lxb_char_t *ty = lxb_dom_element_get_attribute(el, (const lxb_char_t *)"type", 4, &tyl);
    char tb[128];
    size_t b = 0, e = tyl, k;

    if (!ty) return SCRIPT_TYPE_CLASSIC;   /* no type attribute -> "text/javascript" -> classic */
    /* STRIP LEADING AND TRAILING ASCII WHITESPACE, which the spec does before any of the four matches: the
       attribute value is authored text, so `<script type=" module ">` is a module script. Matching the raw
       bytes made it a data block that never ran and never said so. */
    while (b < e && scr_ascii_ws(ty[b])) b++;
    while (e > b && scr_ascii_ws(ty[e - 1])) e--;
    if (e == b) return SCRIPT_TYPE_CLASSIC;   /* empty type attribute -> "text/javascript" -> classic */
    /* A type string longer than this buffer is none of the four matches below: each is an exact ASCII
       case-insensitive compare against a keyword of at most 16 bytes, and a JavaScript MIME type essence is
       shorter still. Truncating it and comparing would answer about a string the element does not have. */
    if (e - b >= sizeof tb) return SCRIPT_TYPE_NONE;
    for (k = b; k < e; k++) { char ch = (char)ty[k]; tb[k - b] = (ch >= 'A' && ch <= 'Z') ? (char)(ch + 32) : ch; }
    tb[e - b] = 0;
    if (strstr(tb, "javascript") || strstr(tb, "ecmascript")) return SCRIPT_TYPE_CLASSIC;
    if (strcmp(tb, "module") == 0)           return SCRIPT_TYPE_MODULE;
    if (strcmp(tb, "importmap") == 0)        return SCRIPT_TYPE_IMPORTMAP;
    if (strcmp(tb, "speculationrules") == 0) return SCRIPT_TYPE_SPECULATIONRULES;
    return SCRIPT_TYPE_NONE;   /* HTML's null: no script is executed */
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
        if (!script_type_executes(script_block_type(el))) continue;   /* data block: not part of JS identity */
        size_t tl = 0; lxb_char_t *txt = lxb_dom_node_text_content(lxb_dom_interface_node(el), &tl);
        if (txt && tl) { for (size_t k = 0; k < tl; k++) { bh ^= txt[k]; bh *= 16777619u; } bh ^= '|'; bh *= 16777619u; }
        if (txt) lxb_dom_document_destroy_text(lxb_dom_interface_node(el)->owner_document, txt);
    }
    free(c.els);
    return bh ? bh : 1;
}

DocScripts document_exec_scripts(lxb_html_document_t *dom) {
    struct scr_ctx c; dom_collect_scripts(dom, &c);
    DocScripts ds = { NULL, NULL, NULL, 0 };
    if (c.n) {
        ds.bodies = calloc((size_t)c.n, sizeof(char *));
        ds.srcs   = calloc((size_t)c.n, sizeof(char *));
        ds.types  = calloc((size_t)c.n, sizeof(ScriptType));
        if (!ds.bodies || !ds.srcs || !ds.types) {
            free(ds.bodies); free(ds.srcs); free(ds.types); free(c.els);
            ds.bodies = ds.srcs = NULL; ds.types = NULL;
            return ds;
        }
    }
    /* Each executable script becomes its OWN entry in document order — never concatenated, so its top-level
       let/const stays script-scoped. An EXTERNAL one takes its position with only a URL; the scheduler parks the
       flow there and the host's reply fills the slot. A data block (json/importmap) is parsed, never run. */
    for (int i = 0; i < c.n; i++) {
        lxb_dom_element_t *el = c.els[i];
        size_t sl = 0;
        const lxb_char_t *src = lxb_dom_element_get_attribute(el, (const lxb_char_t *)"src", 3, &sl);
        ScriptType ty = script_block_type(el);
        if (!script_type_executes(ty)) continue;   /* a data block is parsed, never run */
        if (src && sl) {
            char *u = malloc(sl + 1);
            if (u) { memcpy(u, src, sl); u[sl] = 0; ds.srcs[ds.n] = u; ds.bodies[ds.n] = NULL; ds.types[ds.n] = ty; ds.n++; }
            continue;
        }
        size_t tl = 0; lxb_char_t *txt = lxb_dom_node_text_content(lxb_dom_interface_node(el), &tl);
        if (txt && tl) {
            char *b = malloc(tl + 1);
            if (b) { memcpy(b, txt, tl); b[tl] = 0; ds.types[ds.n] = ty; ds.bodies[ds.n++] = b; }
        }
        if (txt) lxb_dom_document_destroy_text(lxb_dom_interface_node(el)->owner_document, txt);
    }
    free(c.els);
    return ds;
}

void doc_scripts_free(DocScripts *ds) {
    if (!ds || !ds->bodies) return;
    for (int i = 0; i < ds->n; i++) { free(ds->bodies[i]); if (ds->srcs) free(ds->srcs[i]); }
    free(ds->bodies); free(ds->srcs); free(ds->types);
    ds->bodies = ds->srcs = NULL; ds->types = NULL; ds->n = 0;
}
