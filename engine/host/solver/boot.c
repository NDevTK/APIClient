/* Real boot — see boot.h. Uses the real Lexbor HTML parser for spec-faithful <script> extraction. */
#include "solver/boot.h"
#include "check.h"
#include <lexbor/html/html.h>
#include <lexbor/dom/dom.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

struct BootProgram {
    lxb_html_document_t *doc;
    char **scripts;   /* inline <script> sources, document order (owned) */
    int n;
};

BootProgram *boot_parse(const char *html, size_t len) {
    BootProgram *bp = calloc(1, sizeof *bp);
    CHECK(bp, "boot: OOM");
    bp->doc = lxb_html_document_create();
    CHECK(bp->doc, "boot: lxb_html_document_create failed");
    lxb_status_t st = lxb_html_document_parse(bp->doc, (const lxb_char_t *)html, len);
    CHECK(st == LXB_STATUS_OK, "boot: HTML parse failed — the real parser rejected the document");

    lxb_dom_element_t *root = lxb_dom_document_element(&bp->doc->dom_document);
    if (!root) return bp;   /* empty document, no scripts */

    lxb_dom_collection_t *col = lxb_dom_collection_make(&bp->doc->dom_document, 16);
    CHECK(col, "boot: OOM collection");
    lxb_dom_elements_by_tag_name(root, col, (const lxb_char_t *)"script", 6);
    size_t cn = lxb_dom_collection_length(col);
    if (cn) { bp->scripts = calloc(cn, sizeof(char *)); CHECK(bp->scripts, "boot: OOM script list"); }
    for (size_t i = 0; i < cn; i++) {
        lxb_dom_element_t *el = lxb_dom_collection_element(col, i);
        /* inline scripts only for now; an external `src` script is a fetch-then-execute flow (next component). */
        if (lxb_dom_element_has_attribute(el, (const lxb_char_t *)"src", 3)) continue;
        size_t tlen = 0;
        lxb_char_t *txt = lxb_dom_node_text_content(lxb_dom_interface_node(el), &tlen);
        if (txt && tlen) {
            char *s = malloc(tlen + 1);
            CHECK(s, "boot: OOM script source");
            memcpy(s, txt, tlen); s[tlen] = 0;
            bp->scripts[bp->n++] = s;
        }
    }
    lxb_dom_collection_destroy(col, true);
    return bp;
}

void boot_run(JSContext *ctx, void *ud) {
    BootProgram *bp = (BootProgram *)ud;
    for (int i = 0; i < bp->n; i++) {
        JSValue r = JS_Eval(ctx, bp->scripts[i], strlen(bp->scripts[i]), "<script>", JS_EVAL_TYPE_GLOBAL);
        if (JS_IsException(r)) {   /* a page's own throw is the intentional forcing function; surface it */
            JSValue e = JS_GetException(ctx);
            const char *m = JS_ToCString(ctx, e);
            fprintf(stderr, "script %d exception: %s\n", i, m ? m : "?");
            if (m) JS_FreeCString(ctx, m);
            JS_FreeValue(ctx, e);
        }
        JS_FreeValue(ctx, r);
    }
}

void boot_free(BootProgram *bp) {
    if (!bp) return;
    for (int i = 0; i < bp->n; i++) free(bp->scripts[i]);
    free(bp->scripts);
    if (bp->doc) lxb_html_document_destroy(bp->doc);
    free(bp);
}
