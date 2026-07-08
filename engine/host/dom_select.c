/* CSS selector engine over the live Lexbor DOM — see dom_select.h. */
#include "dom_select.h"
#include <lexbor/css/css.h>
#include <lexbor/selectors/selectors.h>
#include <lexbor/dom/dom.h>

extern lxb_html_document_t *g_dom;                             /* the live parsed document (main.c) */
extern JSValue el_wrap(JSContext *ctx, lxb_dom_element_t *el); /* wrap a Lexbor element as a JS object (main.c) */

struct sel_ctx { lxb_dom_element_t *first; };
static lxb_status_t sel_first_cb(lxb_dom_node_t *node, lxb_css_selector_specificity_t s, void *vp) {
    struct sel_ctx *c = vp; (void)s;
    if (!c->first) c->first = lxb_dom_interface_element(node);
    return LXB_STATUS_OK;   /* let the traversal COMPLETE so the selectors object resets cleanly for the next find
                               (returning STOP mid-walk leaves internal state that breaks reuse) */
}
/* Find the first element matching a CSS selector. Fresh selectors object + cleaned parser per call — reusing
   them across finds carries internal state that breaks the next query (2nd querySelector returned null). The
   matched element is owned by g_dom, so tearing down the per-call selectors/list doesn't free it.
   `root` scopes the search: an ELEMENT node -> its subtree (element.querySelector), NULL -> the whole document
   (document.querySelector/getElementById). Document-scoping an element query returns matches OUTSIDE the
   receiver's subtree -> the wrong element -> a wrong learned value; the root fixes that. */
lxb_dom_element_t *dom_select_first(lxb_dom_node_t *root, const char *sel, size_t len) {
    if (!root) root = g_dom ? lxb_dom_interface_node(g_dom) : NULL;
    if (!root) return NULL;
    lxb_css_parser_t *p = lxb_css_parser_create();
    if (!p || lxb_css_parser_init(p, NULL) != LXB_STATUS_OK) { if (p) lxb_css_parser_destroy(p, true); return NULL; }
    lxb_css_selector_list_t *list = lxb_css_selectors_parse(p, (const lxb_char_t *)sel, len);
    if (!list) { lxb_css_parser_destroy(p, true); return NULL; }
    lxb_selectors_t *s = lxb_selectors_create();
    if (!s || lxb_selectors_init(s) != LXB_STATUS_OK) { if (s) lxb_selectors_destroy(s, true); lxb_css_parser_destroy(p, true); return NULL; }
    struct sel_ctx c = { NULL };
    lxb_selectors_find(s, root, list, sel_first_cb, &c);
    lxb_selectors_destroy(s, true);
    lxb_css_parser_destroy(p, true);   /* frees the list too (parser owns it); c.first lives in g_dom */
    return c.first;
}
/* querySelectorAll / getElementsBy* : collect ALL matches into a real array so a for-of/forEach over the
   result iterates real elements (their attrs/methods drive endpoints) — an empty NodeList would starve an
   inline loop body of coverage. Same root-scoping as dom_select_first. */
struct sel_all_ctx { JSContext *ctx; JSValue arr; uint32_t n; };
static lxb_status_t sel_all_cb(lxb_dom_node_t *node, lxb_css_selector_specificity_t s, void *vp) {
    struct sel_all_ctx *c = vp; (void)s;
    JS_SetPropertyUint32(c->ctx, c->arr, c->n++, el_wrap(c->ctx, lxb_dom_interface_element(node)));
    return LXB_STATUS_OK;
}
JSValue dom_select_all(JSContext *ctx, lxb_dom_node_t *root, const char *sel, size_t len) {
    JSValue arr = JS_NewArray(ctx);
    if (!root) root = g_dom ? lxb_dom_interface_node(g_dom) : NULL;
    if (!root) return arr;
    lxb_css_parser_t *p = lxb_css_parser_create();
    if (!p || lxb_css_parser_init(p, NULL) != LXB_STATUS_OK) { if (p) lxb_css_parser_destroy(p, true); return arr; }
    lxb_css_selector_list_t *list = lxb_css_selectors_parse(p, (const lxb_char_t *)sel, len);
    if (!list) { lxb_css_parser_destroy(p, true); return arr; }
    lxb_selectors_t *s = lxb_selectors_create();
    if (!s || lxb_selectors_init(s) != LXB_STATUS_OK) { if (s) lxb_selectors_destroy(s, true); lxb_css_parser_destroy(p, true); return arr; }
    struct sel_all_ctx c = { ctx, arr, 0 };
    lxb_selectors_find(s, root, list, sel_all_cb, &c);
    lxb_selectors_destroy(s, true);
    lxb_css_parser_destroy(p, true);
    return arr;
}
/* REAL element.matches(sel): does THIS element match the selector? A single-node Lexbor match — deterministic
   over the parsed DOM (element state is the app's own DOM, not attacker input), so COMPUTE the real bool
   (RUN, DON'T MATCH), never a stub. closest walks node->parent running the same real match. */
static lxb_status_t sel_match_cb(lxb_dom_node_t *node, lxb_css_selector_specificity_t s, void *vp) { (void)node; (void)s; *(int *)vp = 1; return LXB_STATUS_OK; }
int dom_node_matches(lxb_dom_element_t *el, const char *sel, size_t len) {
    if (!el || !sel) return 0;
    lxb_css_parser_t *p = lxb_css_parser_create();
    if (!p || lxb_css_parser_init(p, NULL) != LXB_STATUS_OK) { if (p) lxb_css_parser_destroy(p, true); return 0; }
    lxb_css_selector_list_t *list = lxb_css_selectors_parse(p, (const lxb_char_t *)sel, len);
    if (!list) { lxb_css_parser_destroy(p, true); return 0; }
    lxb_selectors_t *s = lxb_selectors_create();
    if (!s || lxb_selectors_init(s) != LXB_STATUS_OK) { if (s) lxb_selectors_destroy(s, true); lxb_css_parser_destroy(p, true); return 0; }
    int matched = 0;
    lxb_selectors_match_node(s, lxb_dom_interface_node(el), list, sel_match_cb, &matched);
    lxb_selectors_destroy(s, true);
    lxb_css_parser_destroy(p, true);
    return matched;
}
