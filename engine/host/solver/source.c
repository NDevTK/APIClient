/* Attacker-source delivery — see source.h. The ONE g_candidate + per-source-encoding path every source shares. */
#include "solver/source.h"
#include "check.h"    /* CHECK — an OOM must crash, never emit a prefix-less/unencoded candidate (a false PoC) */
#include <string.h>
#include <stdlib.h>

extern char *g_candidate;   /* the running @S replay flow's candidate (scheduler); NULL in a normal flow */

/* The browser PERCENT-ENCODES the URL per the WHATWG percent-encode SETS, which DIFFER by component (VERIFIED on
   real Chrome): both encode `< > " space`, but the FRAGMENT set (location.hash) encodes backtick and NOT `'`,
   while the SPECIAL-QUERY set (http/https search) encodes `'` and NOT backtick — and that difference decides
   which raw breakout char-set is live. The caller selects the set via enc_backtick/enc_squote. */
static char *pct_encode_url(const char *s, int enc_backtick, int enc_squote) {
    size_t n = strlen(s); char *o = malloc(n * 3 + 1); CHECK(o, "source-encode-oom: an unencoded candidate is a FALSE PoC"); size_t j = 0;
    static const char hex[] = "0123456789ABCDEF";
    for (size_t i = 0; i < n; i++) { unsigned char c = (unsigned char)s[i];
        if (c == ' ' || c == '"' || c == '<' || c == '>' || (enc_backtick && c == '`') || (enc_squote && c == '\'')) { o[j++] = '%'; o[j++] = hex[c >> 4]; o[j++] = hex[c & 15]; }
        else o[j++] = (char)c; }
    o[j] = 0; return o;
}

JSValue source_candidate(JSContext *ctx, const char *prefix, int url_encode, int enc_backtick, int enc_squote) {
    if (!g_candidate) return JS_UNDEFINED;   /* not a replay flow -> caller returns its concolic source */
    char *enc = url_encode ? pct_encode_url(g_candidate, enc_backtick, enc_squote) : NULL;
    const char *payload = enc ? enc : g_candidate;
    size_t lp = prefix ? strlen(prefix) : 0, lc = strlen(payload);
    char *buf = malloc(lp + lc + 1); CHECK(buf, "source-candidate-oom: a prefix-less candidate would corrupt slice(1)/substring(1) payloads");
    if (lp) memcpy(buf, prefix, lp);
    memcpy(buf + lp, payload, lc + 1);
    free(enc);
    JSValue r = JS_NewString(ctx, buf); free(buf);
    return r;
}
