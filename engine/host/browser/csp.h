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

/* CONCRETE per-vector BYPASS ANALYSIS — not a boolean "blocked". For the modeled inline/eval @S vector under
   the page's effective policy, this names the ACTUAL path an attacker would take, the way a browser-security
   engineer reasons about a CSP: an allowlisted HOST hosting a JSONP/framework gadget, `'strict-dynamic'` (an
   injected script CREATED by an already-trusted script runs), a `'nonce-…'` whose VALUE is leaked in the
   served DOM to reuse, or `'unsafe-inline'` (the vector just runs). Trusted-Types enforcement is reported
   separately (an HTML sink THROWS unless a policy stringifies). `via = "none"` means genuinely blocked for
   every modeled vector (escalation needs a same-origin script host — open redirect / JSONP / upload on
   'self'). Sound: a bypass is only asserted when the policy token that enables it is actually present. */
typedef struct {
    int blocked;          /* the modeled inline (or eval) vector is blocked (no 'unsafe-inline'/'unsafe-eval') */
    const char *via;      /* "unsafe-inline" | "unsafe-eval" | "gadget-host" | "strict-dynamic" | "nonce-reuse" | "none" */
    int strict_dynamic;   /* 'strict-dynamic' present (an injected script created by a trusted script executes) */
    int trusted_types;    /* require-trusted-types-for 'script' enforced (an HTML sink throws w/o a TT policy) */
    int dual_policy;      /* BOTH a header and a <meta> policy are enforced -> the bypass must satisfy both */
    char hosts[256];      /* allowlisted external host/scheme sources — JSONP/framework gadget-host candidates */
    char nonce[128];      /* a nonce VALUE leaked in the served DOM (a reuse candidate), or "" */
    char detail[512];     /* the concrete verdict sentence */
} CspBypass;
void csp_bypass(int is_eval, lxb_html_document_t *dom, CspBypass *out);

/* Record the real HTTP Content-Security-Policy RESPONSE HEADER (from the same-origin fetch(location.href)). */
void csp_set_header(const char *csp);

/* Derive the per-document effective policy: the HTTP header if present, else the FIRST <meta> CSP in `dom`. */
void csp_derive(lxb_html_document_t *dom);

/* Teardown: free the policy strings. */
void csp_free(void);

#endif
