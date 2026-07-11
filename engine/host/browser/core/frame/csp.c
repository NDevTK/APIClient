/* Content-Security-Policy analysis — see csp.h. */
#include <string.h>
#include <stdlib.h>
#include "core/frame/csp.h"
#include "check.h"   /* DCHECK — offensive asserts on this security component's invariants (a wrong CSP verdict gates @S PoC feasibility) */
#include <lexbor/dom/dom.h>

/* The DOM-walk callbacks write FIXED byte counts into CspBypass fields: csp_gadget_cb snprintf's 32 into
   gadget_lib; csp_nonce_cb memcpy's up to 127 bytes + NUL into nonce. Enforce the buffer-size contract at
   COMPILE time so a future field-shrink can never silently overflow a fixed buffer in this security path. */
_Static_assert(sizeof(((CspBypass *)0)->gadget_lib) >= 32,  "csp_gadget_cb writes 32 bytes into gadget_lib");
_Static_assert(sizeof(((CspBypass *)0)->nonce)      >= 128, "csp_nonce_cb writes up to 127 bytes + NUL into nonce");

char *g_csp = NULL;
char *g_header_csp = NULL;
char *g_meta_csp = NULL;   /* the <meta http-equiv> policy — a SEPARATE, independently-enforced policy (not a fallback) */

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

/* The value span of directive `name` in `csp` (the text after the name up to ';'), or NULL. */
static const char *csp_directive(const char *csp, const char *name, size_t *len) {
    if (!csp) return NULL;
    size_t nl = strlen(name);
    for (const char *d = strstr(csp, name); d; d = strstr(d + 1, name)) {
        int at_start = (d == csp) || d[-1] == ';' || d[-1] == ' ';        /* a real directive, not a substring of a source */
        char after = d[nl];
        if (at_start && (after == ' ' || after == ';' || after == 0)) {
            const char *v = d + nl; while (*v == ' ') v++;
            const char *end = strchr(v, ';'); *len = end ? (size_t)(end - v) : strlen(v);
            return v;
        }
    }
    return NULL;
}

/* Scan the served DOM's <script src> for a known CSP SCRIPT-GADGET library (Lekies/Kotowicz): a trusted
   library whose own directive/template processing turns injected MARKUP into execution, bypassing CSP even
   under script-src 'self' with no external host (the trusted library, not attacker script, does the eval).
   AngularJS 1.x ({{}} / ng-app) is the canonical one. */
static lxb_status_t csp_gadget_cb(lxb_dom_node_t *node, void *vctx) {
    char *out = (char *)vctx;
    if (out[0]) return LXB_STATUS_STOP;
    if (node->type != LXB_DOM_NODE_TYPE_ELEMENT) return LXB_STATUS_OK;
    lxb_dom_element_t *el = lxb_dom_interface_element(node);
    size_t nl = 0; const lxb_char_t *nm = lxb_dom_element_qualified_name(el, &nl);
    if (nl != 6 || !nm || memcmp(nm, "script", 6) != 0) return LXB_STATUS_OK;
    size_t sl = 0; const lxb_char_t *src = lxb_dom_element_get_attribute(el, (const lxb_char_t *)"src", 3, &sl);
    if (!src || !sl) return LXB_STATUS_OK;
    static const char *libs[] = { "angular", "polymer", "knockout", "ractive", "aurelia", "vue", "mavo", NULL };
    for (int i = 0; libs[i]; i++) {
        size_t ll = strlen(libs[i]);
        for (size_t j = 0; j + ll <= sl; j++) {
            size_t k = 0; for (; k < ll; k++) { char c = (char)src[j + k]; if (c >= 'A' && c <= 'Z') c += 32; if (c != libs[i][k]) break; }
            if (k == ll) { snprintf(out, 32, "%s", libs[i]); return LXB_STATUS_STOP; }
        }
    }
    return LXB_STATUS_OK;
}

/* Collect the FIRST script nonce value present in the served DOM (a reuse/leak candidate for the PoC). */
static lxb_status_t csp_nonce_cb(lxb_dom_node_t *node, void *vctx) {
    char *out = (char *)vctx;
    if (out[0]) return LXB_STATUS_STOP;
    if (node->type != LXB_DOM_NODE_TYPE_ELEMENT) return LXB_STATUS_OK;
    size_t vl = 0; const lxb_char_t *nc = lxb_dom_element_get_attribute(lxb_dom_interface_element(node), (const lxb_char_t *)"nonce", 5, &vl);
    if (nc && vl && vl < 128) { memcpy(out, nc, vl); out[vl] = 0; }
    return LXB_STATUS_OK;
}

/* Effective script-src token list of ONE policy (script-src else default-src): set the flags/hosts/nonce-req. */
static void csp_scan_directive(const char *csp, int *inline_ok, int *eval_ok, int *strict, int *tt,
                               int *nonce_req, char *hosts, size_t hostcap) {
    if (!csp) return;
    size_t dl = 0; const char *d = csp_directive(csp, "script-src", &dl);
    if (!d) d = csp_directive(csp, "default-src", &dl);
    if (csp_directive(csp, "require-trusted-types-for", &(size_t){0})) *tt = 1;
    if (!d) { *inline_ok = *eval_ok = 1; return; }   /* no script directive -> scripts unrestricted */
    const char *p = d, *e = d + dl;
    while (p < e) {
        while (p < e && *p == ' ') p++;
        const char *t = p; while (p < e && *p != ' ') p++;
        size_t tl = (size_t)(p - t);
        if (tl == 0) continue;
        if (tl == 15 && memcmp(t, "'unsafe-inline'", 15) == 0) *inline_ok = 1;
        else if (tl == 13 && memcmp(t, "'unsafe-eval'", 13) == 0) *eval_ok = 1;
        else if (tl == 16 && memcmp(t, "'strict-dynamic'", 16) == 0) *strict = 1;
        else if (tl > 7 && memcmp(t, "'nonce-", 7) == 0) *nonce_req = 1;
        else if (tl > 6 && memcmp(t, "'sha", 4) == 0) { /* hash: fixed-script only, no injection bypass */ }
        else if (t[0] == '\'') { /* 'self'/'none'/other keyword: not a gadget host */ }
        else {   /* an unquoted token = a host or scheme source -> a gadget-host candidate */
            size_t used = strlen(hosts);
            if (used + tl + 2 < hostcap) { if (used) { hosts[used++] = ','; } memcpy(hosts + used, t, tl); hosts[used + tl] = 0; }
        }
    }
}

/* Does the comma-joined host list contain a bare `*` token (allow-any-host wildcard)? */
static int host_has_star(const char *hosts) {
    for (const char *p = hosts; p && *p; ) {
        const char *c = strchr(p, ','); size_t tl = c ? (size_t)(c - p) : strlen(p);
        if (tl == 1 && p[0] == '*') return 1;
        if (!c) break; p = c + 1;
    }
    return 0;
}

void csp_bypass(int is_eval, lxb_html_document_t *dom, CspBypass *out) {
    DCHECK(out, "csp_bypass: NULL out — the @S emitter always passes a real CspBypass buffer; a NULL is a caller bug, not a state to tolerate");
    memset(out, 0, sizeof *out);
    out->via = "unsafe-inline";
    const char *tok = is_eval ? "unsafe-eval" : "unsafe-inline";
    out->blocked = csp_blocks(tok);
    out->dual_policy = (g_header_csp && g_header_csp[0] && g_meta_csp && g_meta_csp[0]);
    if (!out->blocked) { snprintf(out->detail, sizeof out->detail, "'%s' present -> the vector executes as-is", tok); return; }

    /* Scan BOTH enforced policies; a gadget host is a real bypass ONLY if allowed by every policy (intersection),
       but for the common single-policy case the union is the candidate set. Flags OR across policies for the
       hint; the sound per-vector 'blocked' already came from csp_blocks (both policies). */
    int inl = 0, evl = 0, strict = 0, tt = 0, nreq = 0;
    char hh[256] = "", hm[256] = "";
    if (g_header_csp) csp_scan_directive(g_header_csp, &inl, &evl, &strict, &tt, &nreq, hh, sizeof hh);
    if (g_meta_csp)   csp_scan_directive(g_meta_csp,   &inl, &evl, &strict, &tt, &nreq, hm, sizeof hm);
    out->strict_dynamic = strict; out->trusted_types = tt;
    /* Effective gadget-host set = hosts allowed by EVERY enforced policy (a script must satisfy each). A `*`
       source in a policy allows ANY host, so it is the identity for the intersection: `*` in the header does
       not restrict hosts, so the meta's hosts remain candidates (and vice-versa). Both `*` -> any host works. */
    int star_h = host_has_star(hh), star_m = host_has_star(hm);
    if (!out->dual_policy) {
        const char *only = hh[0] ? hh : hm;
        if (host_has_star(only)) snprintf(out->hosts, sizeof out->hosts, "* (any host — host your own script)");
        else snprintf(out->hosts, sizeof out->hosts, "%s", only);
    } else if (star_h && star_m) snprintf(out->hosts, sizeof out->hosts, "* (any host — host your own script)");
    else if (star_h) snprintf(out->hosts, sizeof out->hosts, "%s", hm);   /* header unrestricted -> meta's hosts are the effective set */
    else if (star_m) snprintf(out->hosts, sizeof out->hosts, "%s", hh);
    else { /* both concrete: keep a header host that also appears in the meta list */
        char *save = NULL; size_t o = 0;
        for (char *t = strtok_r(hh, ",", &save); t; t = strtok_r(NULL, ",", &save)) {
            if (strstr(hm, t)) { size_t tl = strlen(t); if (o + tl + 2 < sizeof out->hosts) { if (o) out->hosts[o++] = ','; memcpy(out->hosts + o, t, tl); o += tl; out->hosts[o] = 0; } }
        }
    }
    out->nonce_required = nreq;
    if (dom) lxb_dom_node_simple_walk(lxb_dom_interface_node(dom), csp_gadget_cb, out->gadget_lib);   /* a loaded script-gadget library (bypasses even 'self') */
    if (nreq && dom) lxb_dom_node_simple_walk(lxb_dom_interface_node(dom), csp_nonce_cb, out->nonce);

    /* Order by strength/independence-of-host-policy: a JSONP host and a loaded script-gadget both give direct
       execution; strict-dynamic needs a script-creating gadget; a bare nonce is PROTECTIVE (not a plain reuse). */
    if (out->hosts[0])           { out->via = "gadget-host";    snprintf(out->detail, sizeof out->detail, "script-src allows host(s) %s -> a JSONP endpoint on one (…?callback=payload) returns attacker-controlled JS: <script src=//HOST/jsonp?callback=…>", out->hosts); }
    else if (out->gadget_lib[0]) { out->via = "script-gadget";  snprintf(out->detail, sizeof out->detail, "the page loads '%s', a CSP script-gadget library -> injected MARKUP (e.g. AngularJS ng-app {{…}}) is executed by the TRUSTED library, bypassing CSP with NO attacker <script> — works even under script-src 'self'", out->gadget_lib); }
    else if (out->strict_dynamic){ out->via = "strict-dynamic"; snprintf(out->detail, sizeof out->detail, "'strict-dynamic' -> a script INJECTED by an already-trusted (nonced) script executes; needs a script-creating gadget in the trusted set (document.createElement('script') reached from trusted code)"); }
    else if (nreq)               { out->via = "nonce-protected"; snprintf(out->detail, sizeof out->detail,
        "nonce-protected: inline injection is BLOCKED and a per-request nonce is NOT leakable via injection — the payload is fixed before the nonce is known; nonce-hiding empties the content attribute so getAttribute('nonce') AND the CSS [nonce^=…] attribute-selector side-channel are both neutralised; the element.nonce IDL property still exposes it but needs JS execution you don't yet have. Real bypasses: a STATIC-nonce misconfig (%s%s), 'strict-dynamic' + a script-creating gadget, or a loaded script-gadget library — none of which reuse the nonce",
        out->nonce[0] ? "observed nonce " : "no nonce observed in this response", out->nonce); }
    else                         { out->via = "none";          snprintf(out->detail, sizeof out->detail, "no bypass for the inline/eval vector under this policy%s -> escalation needs a same-origin script host ('self': open redirect / JSONP / uploaded .js)", out->dual_policy ? " (header AND meta both enforced)" : ""); }
    if (out->trusted_types && !is_eval) {
        size_t o = strlen(out->detail);
        snprintf(out->detail + o, sizeof out->detail - o, " | Trusted Types enforced: the HTML sink THROWS unless the payload becomes TrustedHTML — abuse an existing permissive policy, especially a 'default' policy (auto-applies to every sink) or a named policy whose createHTML is identity/weak");
    }
}

void csp_set_header(const char *csp) {
    free(g_header_csp);
    g_header_csp = (csp && csp[0]) ? strdup(csp) : NULL;
}

/* Is a vector requiring `tok` BLOCKED by the page's effective policy? The browser enforces EVERY delivered
   policy INDEPENDENTLY — the HTTP header AND the <meta> policy are BOTH active, and a resource must satisfy
   ALL of them — so the vector is blocked when ANY policy lacks `tok`. A permissive header does NOT rescue a
   restrictive <meta> (the exact false-negative of the old header-OR-meta model). */
int csp_blocks(const char *tok) {
    return (g_header_csp && csp_lacks(g_header_csp, tok)) || (g_meta_csp && csp_lacks(g_meta_csp, tok));
}

void csp_derive(lxb_html_document_t *dom) {
    free(g_csp); g_csp = NULL;   /* fresh document: re-derive its EFFECTIVE policy (per-document) */
    free(g_meta_csp); g_meta_csp = NULL;
    if (dom) lxb_dom_node_simple_walk(lxb_dom_interface_node(dom), csp_scan_cb, &g_meta_csp);   /* ALWAYS scan <meta> — it is a policy the browser enforces alongside the header, not a fallback */
    /* g_csp is the DISPLAY of the effective enforced set (BOTH policies); the per-vector decision is csp_blocks,
       which tests each policy separately (never this concatenation). */
    const char *h = g_header_csp && g_header_csp[0] ? g_header_csp : NULL, *m = g_meta_csp;
    if (h && m) { size_t n = strlen(h) + strlen(m) + 16; g_csp = malloc(n); if (g_csp) snprintf(g_csp, n, "%s [+meta: %s]", h, m); }
    else if (h) g_csp = strdup(h);
    else if (m) g_csp = strdup(m);
}

void csp_free(void) {
    free(g_csp); g_csp = NULL;
    free(g_header_csp); g_header_csp = NULL;
    free(g_meta_csp); g_meta_csp = NULL;
}
