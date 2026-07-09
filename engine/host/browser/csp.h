/* Content-Security-Policy analysis — the page's EFFECTIVE policy and the per-sink-class relevance test that
 * makes an @S finding POLICY-RELATIVE (a real breakout the page's CSP would still block on real Chrome). A
 * self-contained component: it owns the policy string, derives it from the real HTTP header or the <meta> scan
 * of the Lexbor DOM, and answers "is this vector blocked?" — no scheduler / flow state. */
#ifndef ENGINE_HOST_CSP_H
#define ENGINE_HOST_CSP_H

#include <lexbor/html/html.h>

/* The page's EFFECTIVE Content-Security-Policy — the real HTTP response header (PRIMARY) when present, else
   the <meta http-equiv> policy — and the raw HTTP header it derives from. Owned here; read by the @S emitter. */
extern char *g_csp;
extern char *g_header_csp;

/* Does the effective script directive (script-src, else default-src) of ONE policy LACK keyword `tok`? */
int csp_lacks(const char *csp, const char *tok);

/* Is a vector requiring `tok` blocked by the page's EFFECTIVE policy set? The browser enforces the HTTP
   header AND the <meta> policy independently, so the vector is blocked when EITHER lacks `tok`. Use this
   (not csp_lacks on g_csp) for the per-sink verdict. An inline vector needs 'unsafe-inline'; eval 'unsafe-eval'. */
int csp_blocks(const char *tok);

/* Record the real HTTP Content-Security-Policy RESPONSE HEADER (from the same-origin fetch(location.href)). */
void csp_set_header(const char *csp);

/* Derive the per-document effective policy: the HTTP header if present, else the FIRST <meta> CSP in `dom`. */
void csp_derive(lxb_html_document_t *dom);

/* Teardown: free the policy strings. */
void csp_free(void);

#endif
