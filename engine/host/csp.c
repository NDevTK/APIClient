/* Content-Security-Policy analysis — see csp.h. */
#include <string.h>
#include <stdlib.h>
#include "csp.h"
#include <lexbor/dom/dom.h>

char *g_csp = NULL;
char *g_header_csp = NULL;

/* Scan the parsed DOM for the FIRST <meta http-equiv="Content-Security-Policy" content="…">. The real Lexbor
   DOM (never a regex), like the bundle-id script scan. First policy wins (multiple metas -> browser enforces
   the intersection; the first is the overwhelmingly common case). */
static lxb_status_t csp_scan_cb(lxb_dom_node_t *node, void *vctx) {
    char **out = (char **)vctx;
    if (*out) return LXB_STATUS_STOP;
    if (node->type != LXB_DOM_NODE_TYPE_ELEMENT) return LXB_STATUS_OK;
    lxb_dom_element_t *el = lxb_dom_interface_element(node);
    size_t nl = 0; const lxb_char_t *nm = lxb_dom_element_qualified_name(el, &nl);
    if (nl != 4 || !nm || memcmp(nm, "meta", 4) != 0) return LXB_STATUS_OK;
    size_t hl = 0; const lxb_char_t *he = lxb_dom_element_get_attribute(el, (const lxb_char_t *)"http-equiv", 10, &hl);
    if (!he || hl != 23) return LXB_STATUS_OK;   /* strlen("content-security-policy") == 23 */
    static const char want[] = "content-security-policy";
    for (size_t k = 0; k < 23; k++) { char ch = (char)he[k]; if (ch >= 'A' && ch <= 'Z') ch += 32; if (ch != want[k]) return LXB_STATUS_OK; }
    size_t cl = 0; const lxb_char_t *cv = lxb_dom_element_get_attribute(el, (const lxb_char_t *)"content", 7, &cl);
    if (cv && cl) *out = strndup((const char *)cv, cl);
    return LXB_STATUS_OK;
}

/* Does the effective script directive (script-src, else default-src) LACK keyword `tok`? I.e. is a vector that
   requires that keyword BLOCKED? TRUE when the directive is present and does not contain `tok`. No such
   directive -> scripts unrestricted -> not blocked. This is the CSP relevance test per SINK CLASS: an inline
   vector (inline <script>/on* handler/javascript: URL) needs 'unsafe-inline'; an eval vector (eval/Function/
   setTimeout(string)) needs 'unsafe-eval' — a CSP may permit one and forbid the other, so the @S finding must
   report the constraint for ITS vector, not a blanket 'unsafe-inline'. Minimal but sound (a nonce/hash/
   strict-dynamic policy without the keyword still blocks the blanket inline/eval vector we model). */
int csp_lacks(const char *csp, const char *tok) {
    if (!csp) return 0;
    const char *d = strstr(csp, "script-src");
    if (!d) d = strstr(csp, "default-src");
    if (!d) return 0;
    const char *end = strchr(d, ';'); size_t dl = end ? (size_t)(end - d) : strlen(d);
    size_t tl = strlen(tok);
    for (size_t i = 0; i + tl <= dl; i++) if (memcmp(d + i, tok, tl) == 0) return 0;
    return 1;
}

void csp_set_header(const char *csp) {
    free(g_header_csp);
    g_header_csp = (csp && csp[0]) ? strdup(csp) : NULL;
}

void csp_derive(lxb_html_document_t *dom) {
    free(g_csp); g_csp = NULL;   /* fresh document: re-derive its EFFECTIVE policy (per-document) */
    if (g_header_csp && g_header_csp[0]) {
        g_csp = strdup(g_header_csp);   /* the REAL HTTP CSP header — the primary policy the browser enforces */
    } else if (dom) {
        lxb_dom_node_simple_walk(lxb_dom_interface_node(dom), csp_scan_cb, &g_csp);   /* no header CSP: fall back to <meta> (static hosting) */
    }
}

void csp_free(void) {
    free(g_csp); g_csp = NULL;
    free(g_header_csp); g_header_csp = NULL;
}
