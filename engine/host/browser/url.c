/* URL query-parameter extraction + {src}-hole value-solve + WHATWG url_resolve — see url.h. */
#include <string.h>
#include <stdlib.h>
#include "url.h"
#include "constraints.h"   /* cons_fixed_value — an == gate PINS a {src} hole to its concrete value */
#include "check.h"         /* CHECK — OOM must crash LOUD, never silently degrade a solved URL back to its shape */
#include <lexbor/url/url.h>   /* the real WHATWG URL Standard parser — url_resolve canonicalizes here, not a hand-rolled resolver */

/* Resolve a URL with the vendored LEXBOR URL module (the real WHATWG URL Standard parser) — never a
   hand-rolled string resolver. Returns the serialized absolute href (malloc'd; caller frees) or NULL on a
   parse failure (-> the caller yields opaque, never an invented value). Browser URL canonicalization owns no
   scheduler/flow state, so it lives with the other URL components, not in the scheduler (main.c). */
struct url_ser_buf { char *s; size_t n, cap; };
static lxb_status_t url_ser_cb(const lxb_char_t *data, size_t len, void *cbctx) {
    struct url_ser_buf *b = cbctx;
    if (b->n + len + 1 > b->cap) { size_t nc = (b->n + len + 1) * 2 + 64; char *ns = realloc(b->s, nc); if (!ns) return LXB_STATUS_ERROR_MEMORY_ALLOCATION; b->s = ns; b->cap = nc; }
    memcpy(b->s + b->n, data, len); b->n += len; b->s[b->n] = 0;
    return LXB_STATUS_OK;
}
char *url_resolve(const char *input, const char *base) {
    lxb_url_parser_t *p = lxb_url_parser_create();
    if (!p || lxb_url_parser_init(p, NULL) != LXB_STATUS_OK) { if (p) lxb_url_parser_destroy(p, true); return NULL; }
    lxb_url_t *bu = (base && base[0]) ? lxb_url_parse(p, NULL, (const lxb_char_t *)base, strlen(base)) : NULL;
    lxb_url_t *u = lxb_url_parse(p, bu, (const lxb_char_t *)(input ? input : ""), input ? strlen(input) : 0);
    char *out = NULL;
    if (u) { struct url_ser_buf b = {0}; if (lxb_url_serialize(u, url_ser_cb, &b, false) == LXB_STATUS_OK) out = b.s; else free(b.s); }
    lxb_url_parser_destroy(p, true);   /* frees bu, u, and internal buffers */
    return out;
}

/* Substitute each `{src}` hole the running flow FIXED (== gate) with its concrete value, so a URL built from
   gated input surfaces the SOLVED key in BOTH path and query (/api/{hash} -> /api/admin). NULL if nothing
   solved. The @H shape is re-derived downstream, so grouping is unaffected — only the example gains a value. */
char *url_solve_holes(JSContext *ctx, const char *url) {
    (void)ctx;
    if (!url || !strchr(url, '{')) return NULL;
    size_t cap = strlen(url) + 64, len = 0; char *out = malloc(cap); CHECK(out, "url-solve-oom: malloc failed -> a solved URL would silently degrade to its unconcretized shape");
    int changed = 0;
    for (const char *p = url; *p; ) {
        if (*p == '{') {
            const char *close = strchr(p, '}');
            if (close && (size_t)(close - p + 1) < 64) {
                char hole[64]; size_t hl = (size_t)(close - p + 1); memcpy(hole, p, hl); hole[hl] = 0;
                const char *fixed = cons_fixed_value(hole);
                if (fixed) { size_t fl = strlen(fixed);
                    while (len + fl + 1 > cap) { cap *= 2; char *n = realloc(out, cap); CHECK(n, "url-solve-oom: realloc failed"); out = n; }
                    memcpy(out + len, fixed, fl); len += fl; p = close + 1; changed = 1; continue; }
            }
        }
        if (len + 2 > cap) { cap *= 2; char *n = realloc(out, cap); CHECK(n, "url-solve-oom: realloc failed"); out = n; }
        out[len++] = *p++;
    }
    out[len] = 0;
    if (!changed) { free(out); return NULL; }
    return out;
}

/* Read a network web API's URL argument (see url.h): the opaque EXAMPLE (a computed/config URL the flow
   pinned) takes precedence over a raw ToString, so `new WebSocket(cfg.wsUrl)` records the concrete endpoint. */
char *url_from_arg(JSContext *ctx, JSValueConst arg) {
    JSValue ex = JS_OpaqueExample(ctx, arg); const char *u = NULL;
    if (!JS_IsUndefined(ex)) u = JS_ToCString(ctx, ex);
    if (!u) u = JS_ToCString(ctx, arg);
    char *r = (u && u[0]) ? strdup(u) : NULL;
    if (u) JS_FreeCString(ctx, u);
    JS_FreeValue(ctx, ex);
    return r;
}

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
