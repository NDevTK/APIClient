/* Milestone-1 falsifiable gate: does Lexbor (spec HTML5 parser + DOM +
   CSS selectors) compile under emcc to wasm AND run? Parse real HTML,
   run a CSS selector, read an attribute off the matched element. If
   this prints the href under node-wasm, Lexbor is viable in-engine. */
#include <lexbor/html/html.h>
#include <lexbor/css/css.h>
#include <lexbor/selectors/selectors.h>
#include <stdio.h>

static lxb_status_t
find_cb(lxb_dom_node_t *node, lxb_css_selector_specificity_t s, void *ctx)
{
    lxb_dom_element_t *el = lxb_dom_interface_element(node);
    size_t len = 0;
    const lxb_char_t *href =
        lxb_dom_element_get_attribute(el, (const lxb_char_t *) "href", 4, &len);
    printf("MATCH href=%.*s\n",
           (int) (href ? len : 0), href ? (const char *) href : "");
    return LXB_STATUS_OK;
}

int main(void)
{
    static const lxb_char_t html[] =
        "<html><body><div id=app>"
        "<a class=lnk href=\"https://api.x.com/v1/u?id=7\">L</a>"
        "</div></body></html>";
    static const lxb_char_t q[] = "a.lnk";

    lxb_html_document_t *doc = lxb_html_document_create();
    if (doc == NULL ||
        lxb_html_document_parse(doc, html, sizeof(html) - 1) != LXB_STATUS_OK) {
        printf("PARSE_FAIL\n"); return 1;
    }
    lxb_css_parser_t *p = lxb_css_parser_create();
    if (lxb_css_parser_init(p, NULL) != LXB_STATUS_OK) { printf("CSSP_FAIL\n"); return 1; }
    lxb_selectors_t *sel = lxb_selectors_create();
    if (lxb_selectors_init(sel) != LXB_STATUS_OK) { printf("SEL_FAIL\n"); return 1; }

    lxb_css_selector_list_t *list = lxb_css_selectors_parse(p, q, sizeof(q) - 1);
    if (list == NULL) { printf("SEL_PARSE_FAIL\n"); return 1; }

    lxb_dom_node_t *body = lxb_dom_interface_node(lxb_html_document_body_element(doc));
    lxb_status_t st = lxb_selectors_find(sel, body, list, find_cb, NULL);
    printf("DONE status=%d\n", (int) st);
    return 0;
}
