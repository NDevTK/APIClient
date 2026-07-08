/* URL query-parameter extraction — see url.h. */
#include <string.h>
#include "url.h"

/* A URL/shape carries an opaque HOLE — "{}" (generic) or "{tag}" (source-tagged: {hash}/{search}) — iff it
   has a '{' followed by only lowercase letters then '}'. Such a URL is not concretely fetchable. Generalizes
   the old literal strstr("{}") checks so source-tagged holes are still recognized as opaque. */
int has_hole(const char *s) {
    if (!s) return 0;
    for (const char *p = s; (p = strchr(p, '{')); p++) {
        const char *q = p + 1; while (*q >= 'a' && *q <= 'z') q++;
        if (*q == '}') return 1;
    }
    return 0;
}
static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
/* Percent-decode a URL component (decodeURIComponent-ish: %XX -> byte; newlines normalized to spaces). */
static void url_pct_decode(const char *s, size_t n, char *out, size_t outcap) {
    size_t o = 0;
    for (size_t i = 0; i < n && o + 1 < outcap; i++) {
        int hi, lo;
        if (s[i] == '%' && i + 2 < n
            && (hi = hexval(s[i+1])) >= 0 && (lo = hexval(s[i+2])) >= 0) {
            int c = (hi << 4) | lo; out[o++] = (c == '\n' || c == '\r') ? ' ' : (char)c; i += 2;
        } else {
            out[o++] = (s[i] == '\n' || s[i] == '\r') ? ' ' : s[i];
        }
    }
    out[o] = 0;
}
/* Build query-param objects ({name,location:"query",validValues:[value?]}) from a COMPUTED url onto the
   endpoint's params array. The engine owns URL parsing (Lexbor-canonical); the host never re-splits the
   URL string. A hole value ({search}) passes through literally (opacity marker, decoded downstream). */
void build_query_params(JSContext *ctx, const char *url, JSValueConst params) {
    if (!url) return;
    const char *q = strchr(url, '?'); if (!q) return;
    q++;
    const char *end = strchr(q, '#'); if (!end) end = q + strlen(q);
    uint32_t idx = 0;
    for (const char *p = q; p < end; ) {
        const char *amp = memchr(p, '&', (size_t)(end - p)); if (!amp) amp = end;
        const char *eq = memchr(p, '=', (size_t)(amp - p));
        const char *ne = eq ? eq : amp;
        const char *vb = eq ? eq + 1 : amp;
        char nbuf[256], vbuf[512];
        url_pct_decode(p, (size_t)(ne - p), nbuf, sizeof nbuf);
        url_pct_decode(vb, (size_t)(amp - vb), vbuf, sizeof vbuf);
        if (nbuf[0]) {
            JSValue po = JS_NewObject(ctx);
            JS_SetPropertyStr(ctx, po, "name", JS_NewString(ctx, nbuf));
            JS_SetPropertyStr(ctx, po, "location", JS_NewString(ctx, "query"));
            JSValue vv = JS_NewArray(ctx);
            if (vbuf[0]) JS_SetPropertyUint32(ctx, vv, 0, JS_NewString(ctx, vbuf));   /* eurl already value-solved upstream */
            JS_SetPropertyStr(ctx, po, "validValues", vv);
            JS_SetPropertyUint32(ctx, params, idx++, po);
        }
        p = (amp < end) ? amp + 1 : end;
    }
}
