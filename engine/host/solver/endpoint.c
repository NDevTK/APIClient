/* The @H endpoint sink — see endpoint.h.
 *
 * HOST FINDINGS ARE C DATA — NOT JS-HEAP OBJECTS. A learned endpoint is stored in a typed C struct
 * (Endpoint), fully OUTSIDE the JS heap, so it is COW-invisible BY CONSTRUCTION: the per-flow COW delta
 * captures baseline page-heap objects, and a C struct is not one, so it can never be captured/reverted when
 * the recording flow parks (an awaited fetch — heavy_async) or hands off. This is the principled replacement
 * for the deleted per-object `cow_exempt` flag (a manual opt-out easy to forget): host analysis state simply
 * does not live in the captured heap. `record_endpoint` EXTRACTS the transient JS `ep` into the C struct at
 * record time (when its values are live, before any revert); `endpoint_snapshot` rebuilds a JS array for the
 * in-engine dedup at emit (transient, flow_local, never captured). */
#include "solver/endpoint.h"
#include "check.h"            /* CHECK (OOM = a dropped finding corrupts the moat) + DCHECK (invariants) */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Borrowed from main.c (the scheduler side): the @S candidate flag (a candidate flow's requests are @S
   artifacts, not real @H), the emit counter, and the flow value-emit signal. */
extern char *g_candidate;
extern int g_emit_total;
extern void flow_emit_value(void);

typedef struct { char *name; char *location; char **vals; int vals_n; } EpParam;
typedef struct { char *name; char *value; } EpHeader;
typedef struct {
    char *method, *url, *source, *body;
    EpParam *params; int params_n;
    EpHeader *headers; int headers_n;
} Endpoint;

static Endpoint *g_eps = NULL;      /* the learned @H endpoints — C data, COW-invisible by construction */
static int g_eps_n = 0, g_eps_cap = 0;

/* dup a string-valued own property to a heap C string (NULL if absent/non-string). */
static char *ep_str_prop(JSContext *ctx, JSValueConst o, const char *k) {
    JSValue v = JS_GetPropertyStr(ctx, o, k);
    char *r = NULL;
    if (JS_IsString(v)) { const char *s = JS_ToCString(ctx, v); if (s) { r = strdup(s); JS_FreeCString(ctx, s); } }
    JS_FreeValue(ctx, v);
    return r;
}

/* The shared @H sink: record one endpoint (consumes `ep`) + signal the emit. A CANDIDATE flow carries a
   concrete @S breakout PAYLOAD, so its request URLs are @S artifacts, NOT real @H — only OPAQUE flows emit.
   fetch AND XMLHttpRequest funnel through here so XHR-based apps are learned like fetch ones. The JS `ep` is
   EXTRACTED into a C Endpoint here and then freed — nothing of the finding survives in the JS heap. */
void record_endpoint(JSContext *ctx, JSValue ep) {
    if (!g_candidate && JS_IsObject(ep)) {
        if (g_eps_n >= g_eps_cap) { g_eps_cap = g_eps_cap ? g_eps_cap * 2 : 16; g_eps = realloc(g_eps, (size_t)g_eps_cap * sizeof(Endpoint)); }
        Endpoint *e = &g_eps[g_eps_n];
        memset(e, 0, sizeof *e);
        e->method = ep_str_prop(ctx, ep, "method");
        e->url    = ep_str_prop(ctx, ep, "url");
        e->source = ep_str_prop(ctx, ep, "source");
        e->body   = ep_str_prop(ctx, ep, "body");
        /* params: [{name, location, validValues:[...]}] */
        JSValue params = JS_GetPropertyStr(ctx, ep, "params");
        if (JS_IsArray(params)) {
            uint32_t pn = 0; JSValue plv = JS_GetPropertyStr(ctx, params, "length"); JS_ToUint32(ctx, &pn, plv); JS_FreeValue(ctx, plv);
            if (pn) e->params = calloc(pn, sizeof(EpParam));
            for (uint32_t i = 0; i < pn; i++) {
                JSValue po = JS_GetPropertyUint32(ctx, params, i);
                EpParam *p = &e->params[e->params_n];
                p->name = ep_str_prop(ctx, po, "name");
                p->location = ep_str_prop(ctx, po, "location");
                JSValue vv = JS_GetPropertyStr(ctx, po, "validValues");
                if (JS_IsArray(vv)) {
                    uint32_t vn = 0; JSValue vlv = JS_GetPropertyStr(ctx, vv, "length"); JS_ToUint32(ctx, &vn, vlv); JS_FreeValue(ctx, vlv);
                    if (vn) p->vals = calloc(vn, sizeof(char *));
                    for (uint32_t j = 0; j < vn; j++) {
                        JSValue el = JS_GetPropertyUint32(ctx, vv, j);
                        if (JS_IsString(el)) { const char *s = JS_ToCString(ctx, el); if (s) { p->vals[p->vals_n++] = strdup(s); JS_FreeCString(ctx, s); } }
                        JS_FreeValue(ctx, el);
                    }
                }
                JS_FreeValue(ctx, vv);
                e->params_n++;
                JS_FreeValue(ctx, po);
            }
        }
        JS_FreeValue(ctx, params);
        /* headers: {name: value} */
        JSValue hdrs = JS_GetPropertyStr(ctx, ep, "headers");
        if (JS_IsObject(hdrs)) {
            JSPropertyEnum *tab = NULL; uint32_t hn = 0;
            if (JS_GetOwnPropertyNames(ctx, &tab, &hn, hdrs, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) == 0) {
                if (hn) e->headers = calloc(hn, sizeof(EpHeader));
                for (uint32_t i = 0; i < hn; i++) {
                    const char *hk = JS_AtomToCString(ctx, tab[i].atom);
                    JSValue hv = JS_GetProperty(ctx, hdrs, tab[i].atom);
                    const char *hvs = JS_IsString(hv) ? JS_ToCString(ctx, hv) : NULL;
                    if (hk && hvs) { EpHeader *h = &e->headers[e->headers_n++]; h->name = strdup(hk); h->value = strdup(hvs); }
                    if (hk) JS_FreeCString(ctx, hk);
                    if (hvs) JS_FreeCString(ctx, hvs);
                    JS_FreeValue(ctx, hv);
                }
                JS_FreePropertyEnum(ctx, tab, hn);
            }
        }
        JS_FreeValue(ctx, hdrs);
        g_eps_n++;
        g_emit_total++;
    }
    JS_FreeValue(ctx, ep);
    flow_emit_value();   /* raise the running flow's value (the WFQ progress signal) */
}

/* ── in-engine @H dedup/aggregation (the cumulative moat aggregation the ENGINE owns) ──────────────────
   Formerly the self-hosted DEDUP_JS prelude; now C, operating DIRECTLY on the C findings — the engine owns
   this analysis, not a self-hosted JS layer run once at emit (unlike the Array prelude, DEDUP had no
   opacity/trampolining reason to be JS). Faithful to the prior four phases: (1) body -> params, (2) merge by
   method + hole-normalized url, (3) path-hole collapse (a concrete url folds into a {hole} shape as path
   params), (4) query-phantom collapse (a {} query value dominated by a concrete sibling). The body codec uses
   the engine's REAL JSON (JS_ParseJSON) — never a hand-rolled parser (a codec is the browser/JS half). */

static int ep_has_hole(const char *s) {   /* any whole {[a-z]*} hole anywhere in s */
    for (const char *p = s ? s : ""; (p = strchr(p, '{')); p++) {
        const char *q = p + 1; while (*q >= 'a' && *q <= 'z') q++;
        if (*q == '}') return 1;
    }
    return 0;
}
static int ep_seg_is_hole(const char *s) {   /* a segment that is ENTIRELY one hole: ^{[a-z]*}$ */
    if (!s || s[0] != '{') return 0;
    const char *q = s + 1; while (*q >= 'a' && *q <= 'z') q++;
    return q[0] == '}' && q[1] == 0;
}
static char *ep_norm_holes(const char *s) {   /* strdup(s) with every {[a-z]*} run rewritten to {} */
    size_t n = strlen(s ? s : "");
    char *out = malloc(n + 1); CHECK(out, "OOM: endpoint url normalize"); size_t o = 0;
    for (const char *p = s ? s : ""; *p; ) {
        if (*p == '{') { const char *q = p + 1; while (*q >= 'a' && *q <= 'z') q++;
            if (*q == '}') { out[o++] = '{'; out[o++] = '}'; p = q + 1; continue; } }
        out[o++] = *p++;
    }
    out[o] = 0; return out;
}

/* a mutable merge-endpoint (owned copy) — the working set the dedup mutates; g_eps is left untouched so a
   later incremental emit re-dedups from the raw findings. */
typedef struct { EpParam *params; int params_n; EpHeader *headers; int headers_n; char *method, *url, *nurl, *body; int dropped; } MEp;

static EpParam *mep_find_param(MEp *m, const char *name, const char *loc) {
    for (int i = 0; i < m->params_n; i++)
        if (!strcmp(m->params[i].name ? m->params[i].name : "", name) && !strcmp(m->params[i].location ? m->params[i].location : "", loc)) return &m->params[i];
    return NULL;
}
static EpParam *mep_add_param(MEp *m, const char *name, const char *loc) {
    m->params = realloc(m->params, (size_t)(m->params_n + 1) * sizeof(EpParam)); CHECK(m->params, "OOM: endpoint param");
    EpParam *p = &m->params[m->params_n++]; memset(p, 0, sizeof *p);
    p->name = strdup(name); p->location = strdup(loc); return p;
}
static void ep_val_add(EpParam *p, const char *v) {   /* union a validValue (dedup) */
    for (int i = 0; i < p->vals_n; i++) if (!strcmp(p->vals[i], v)) return;
    p->vals = realloc(p->vals, (size_t)(p->vals_n + 1) * sizeof(char *)); CHECK(p->vals, "OOM: endpoint validValue");
    p->vals[p->vals_n++] = strdup(v);
}
static EpHeader *mep_find_header(MEp *m, const char *name) {
    for (int i = 0; i < m->headers_n; i++) if (!strcmp(m->headers[i].name, name)) return &m->headers[i];
    return NULL;
}
static void mep_add_header(MEp *m, const char *name, const char *value) {
    m->headers = realloc(m->headers, (size_t)(m->headers_n + 1) * sizeof(EpHeader)); CHECK(m->headers, "OOM: endpoint header");
    m->headers[m->headers_n].name = strdup(name); m->headers[m->headers_n].value = strdup(value); m->headers_n++;
}
static void mep_from(MEp *m, Endpoint *e) {
    memset(m, 0, sizeof *m);
    m->method = strdup(e->method ? e->method : "GET");
    m->url = strdup(e->url ? e->url : "?");
    m->nurl = ep_norm_holes(m->url);
    if (e->body) m->body = strdup(e->body);
    for (int i = 0; i < e->params_n; i++) {
        EpParam *p = mep_add_param(m, e->params[i].name ? e->params[i].name : "", e->params[i].location ? e->params[i].location : "query");
        for (int j = 0; j < e->params[i].vals_n; j++) ep_val_add(p, e->params[i].vals[j]);
    }
    for (int i = 0; i < e->headers_n; i++) mep_add_header(m, e->headers[i].name, e->headers[i].value);
}
static void mep_free1(MEp *m) {
    free(m->method); free(m->url); free(m->nurl); free(m->body);
    for (int i = 0; i < m->params_n; i++) { free(m->params[i].name); free(m->params[i].location); for (int j = 0; j < m->params[i].vals_n; j++) free(m->params[i].vals[j]); free(m->params[i].vals); }
    free(m->params);
    for (int i = 0; i < m->headers_n; i++) { free(m->headers[i].name); free(m->headers[i].value); }
    free(m->headers);
}
/* (1) body -> params: a JSON object body's fields, OR a form-urlencoded body's pairs, become body params. */
static void mep_body_params(JSContext *ctx, MEp *m) {
    if (!m->body || !m->body[0]) return;
    JSValue bo = JS_ParseJSON(ctx, m->body, strlen(m->body), "<body>");
    if (!JS_IsException(bo) && JS_IsObject(bo) && !JS_IsArray(bo)) {
        JSPropertyEnum *tab = NULL; uint32_t bn = 0;
        if (JS_GetOwnPropertyNames(ctx, &tab, &bn, bo, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) == 0) {
            for (uint32_t i = 0; i < bn; i++) {
                const char *bk = JS_AtomToCString(ctx, tab[i].atom);
                JSValue bv = JS_GetProperty(ctx, bo, tab[i].atom);
                if (bk && !mep_find_param(m, bk, "body")) {
                    EpParam *p = mep_add_param(m, bk, "body");
                    int op = JS_IsNull(bv);   /* null | "{}" | empty-object -> no validValue (opaque) */
                    if (!op && JS_IsString(bv)) { const char *sv = JS_ToCString(ctx, bv); if (sv) { if (!strcmp(sv, "{}")) op = 1; JS_FreeCString(ctx, sv); } }
                    if (!op && JS_IsObject(bv) && !JS_IsArray(bv)) { JSPropertyEnum *t2 = NULL; uint32_t n2 = 0; if (JS_GetOwnPropertyNames(ctx, &t2, &n2, bv, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) == 0) { if (n2 == 0) op = 1; JS_FreePropertyEnum(ctx, t2, n2); } }
                    if (!op) {
                        JSValue js = JS_UNDEFINED; const char *vs = NULL;
                        if (JS_IsObject(bv)) { js = JS_JSONStringify(ctx, bv, JS_UNDEFINED, JS_UNDEFINED); if (JS_IsString(js)) vs = JS_ToCString(ctx, js); }
                        else vs = JS_ToCString(ctx, bv);
                        if (vs) { ep_val_add(p, vs); JS_FreeCString(ctx, vs); }
                        JS_FreeValue(ctx, js);
                    }
                }
                if (bk) JS_FreeCString(ctx, bk);
                JS_FreeValue(ctx, bv);
            }
            JS_FreePropertyEnum(ctx, tab, bn);
        }
        JS_FreeValue(ctx, bo);
        return;
    }
    JS_FreeValue(ctx, bo);
    if (strchr(m->body, '=') && m->body[0] != '{' && m->body[0] != '[') {   /* form-urlencoded */
        char *dup = strdup(m->body); CHECK(dup, "OOM: endpoint body");
        for (char *tok = strtok(dup, "&"); tok; tok = strtok(NULL, "&")) {
            char *eq = strchr(tok, '='); if (!eq) continue; *eq = 0;
            const char *fk = tok, *fv = eq + 1;
            if (!mep_find_param(m, fk, "body")) { EpParam *p = mep_add_param(m, fk, "body"); if (!ep_has_hole(fv)) ep_val_add(p, fv); }
        }
        free(dup);
    }
}
/* (2) UNION src into dst (same identity): params (merge validValues), headers (concrete beats hole), body. */
static void mep_merge(MEp *dst, MEp *src) {
    for (int i = 0; i < src->params_n; i++) {
        EpParam *sp = &src->params[i];
        EpParam *f = mep_find_param(dst, sp->name ? sp->name : "", sp->location ? sp->location : "");
        if (!f) f = mep_add_param(dst, sp->name ? sp->name : "", sp->location ? sp->location : "");
        for (int j = 0; j < sp->vals_n; j++) ep_val_add(f, sp->vals[j]);
    }
    for (int i = 0; i < src->headers_n; i++) {
        EpHeader *sh = &src->headers[i]; EpHeader *fh = mep_find_header(dst, sh->name);
        if (!fh) mep_add_header(dst, sh->name, sh->value);
        else if (ep_has_hole(fh->value) && !ep_has_hole(sh->value)) { free(fh->value); fh->value = strdup(sh->value); }
    }
    if (src->body && (!dst->body || (ep_has_hole(dst->body) && !ep_has_hole(src->body)))) { free(dst->body); dst->body = strdup(src->body); }
}

/* path/query segmentation (mirrors DEDUP_JS pathSegs + parseQ) */
typedef struct { char **seg; int n; char *query; char *buf; } Segs;
static void segs_of(const char *url, Segs *s) {
    memset(s, 0, sizeof *s);
    const char *q = strchr(url, '?');
    size_t plen = q ? (size_t)(q - url) : strlen(url);
    s->query = strdup(q ? q : "");
    s->buf = malloc(plen + 1); CHECK(s->buf, "OOM: endpoint path"); memcpy(s->buf, url, plen); s->buf[plen] = 0;
    int cap = 8; s->seg = malloc((size_t)cap * sizeof(char *)); CHECK(s->seg, "OOM: endpoint segs");
    for (char *p = s->buf;;) {   /* split on '/' preserving empty segments (JS split semantics) */
        if (s->n >= cap) { cap *= 2; s->seg = realloc(s->seg, (size_t)cap * sizeof(char *)); CHECK(s->seg, "OOM: endpoint segs"); }
        char *sl = strchr(p, '/');
        if (sl) { *sl = 0; s->seg[s->n++] = p; p = sl + 1; } else { s->seg[s->n++] = p; break; }
    }
}
static void segs_free(Segs *s) { free(s->seg); free(s->query); free(s->buf); }
typedef struct { char *k, *v; } QKV;
static int parse_q(const char *q, QKV *out, int max) {   /* q leads with '?'; splits into k/v (dup'd, caller frees) */
    int n = 0; if (!q || !q[0]) return 0;
    const char *s0 = q[0] == '?' ? q + 1 : q; if (!s0[0]) return 0;
    char *dup = strdup(s0); CHECK(dup, "OOM: endpoint query");
    for (char *tok = strtok(dup, "&"); tok && n < max; tok = strtok(NULL, "&")) {
        char *eq = strchr(tok, '=');
        if (eq) { *eq = 0; out[n].k = strdup(tok); out[n].v = strdup(eq + 1); } else { out[n].k = strdup(tok); out[n].v = strdup(""); }
        n++;
    }
    free(dup); return n;
}

/* Dedup the C findings into a transient JS array for @RESULT assembly at emit (created outside any capturing
   context, freed by the caller). g_eps is NOT mutated — a later incremental emit re-dedups from the raw set. */
JSValue endpoint_snapshot(JSContext *ctx) {
    int N = g_eps_n;
    MEp *m = N ? calloc((size_t)N, sizeof(MEp)) : NULL;
    if (N) CHECK(m, "OOM: endpoint dedup set");
    for (int i = 0; i < N; i++) { mep_from(&m[i], &g_eps[i]); mep_body_params(ctx, &m[i]); }
    /* (2) merge by method + hole-normalized url — first occurrence is the survivor. */
    for (int i = 0; i < N; i++) { if (m[i].dropped) continue;
        for (int j = i + 1; j < N; j++) { if (m[j].dropped) continue;
            if (!strcmp(m[i].method, m[j].method) && !strcmp(m[i].nurl, m[j].nurl)) { mep_merge(&m[i], &m[j]); m[j].dropped = 1; }
        }
    }
    /* (3) path-hole collapse: a concrete url folds into a {hole}-shape url as arg<idx> path params. */
    for (int ci = 0; ci < N; ci++) { if (m[ci].dropped || ep_has_hole(m[ci].url)) continue;
        Segs cs; segs_of(m[ci].url, &cs);
        for (int si = 0; si < N; si++) { if (m[si].dropped || si == ci || !ep_has_hole(m[si].url) || strcmp(m[si].method, m[ci].method)) continue;
            Segs ss; segs_of(m[si].url, &ss);
            if (ss.n != cs.n || strcmp(ss.query, cs.query)) { segs_free(&ss); continue; }
            int ok = 1, exidx[64], exn = 0; const char *exval[64];
            for (int j = 0; j < ss.n; j++) {
                if (ep_seg_is_hole(ss.seg[j])) { if (cs.seg[j][0] && !ep_has_hole(cs.seg[j])) { if (exn < 64) { exidx[exn] = j; exval[exn] = cs.seg[j]; exn++; } } else { ok = 0; break; } }
                else if (strcmp(ss.seg[j], cs.seg[j])) { ok = 0; break; }
            }
            if (ok && exn) {
                for (int e = 0; e < exn; e++) { char nm[32]; snprintf(nm, sizeof nm, "arg%d", exidx[e]);
                    EpParam *pp = mep_find_param(&m[si], nm, "path"); if (!pp) pp = mep_add_param(&m[si], nm, "path"); ep_val_add(pp, exval[e]); }
                m[ci].dropped = 1; segs_free(&ss); break;
            }
            segs_free(&ss);
        }
        segs_free(&cs);
    }
    /* (4) query-phantom collapse: a {}-valued query key dominated by a concrete sibling drops the phantom. */
    for (int qi = 0; qi < N; qi++) { if (m[qi].dropped) continue;
        Segs ps; segs_of(m[qi].url, &ps);
        if (!strstr(ps.query, "{}")) { segs_free(&ps); continue; }
        QKV pq[32]; int pqn = parse_q(ps.query, pq, 32);
        for (int cj = 0; cj < N; cj++) { if (cj == qi || m[cj].dropped || strcmp(m[cj].method, m[qi].method)) continue;
            Segs cs2; segs_of(m[cj].url, &cs2);
            int samepath = (ps.n == cs2.n); for (int j = 0; samepath && j < ps.n; j++) if (strcmp(ps.seg[j], cs2.seg[j])) samepath = 0;
            if (samepath) {
                QKV cq[32]; int cqn = parse_q(cs2.query, cq, 32);
                if (cqn == pqn) {
                    int ok0 = 1, better = 0;
                    for (int ki = 0; ki < pqn; ki++) {
                        const char *cv = NULL; for (int x = 0; x < cqn; x++) if (!strcmp(cq[x].k, pq[ki].k)) { cv = cq[x].v; break; }
                        if (!cv) { ok0 = 0; break; }
                        if (!strcmp(pq[ki].v, "{}")) { if (strcmp(cv, "{}") && !ep_has_hole(cv)) better = 1; }
                        else if (strcmp(pq[ki].v, cv)) { ok0 = 0; break; }
                    }
                    if (ok0 && better) m[qi].dropped = 1;
                }
                for (int x = 0; x < cqn; x++) { free(cq[x].k); free(cq[x].v); }
            }
            segs_free(&cs2);
            if (m[qi].dropped) break;
        }
        for (int x = 0; x < pqn; x++) { free(pq[x].k); free(pq[x].v); }
        segs_free(&ps);
    }
    /* serialize survivors */
    JSValue arr = JS_NewArray(ctx); uint32_t oi = 0;
    for (int i = 0; i < N; i++) { if (m[i].dropped) continue; MEp *e = &m[i];
        JSValue ep = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, ep, "method", JS_NewString(ctx, e->method));
        JS_SetPropertyStr(ctx, ep, "url", JS_NewString(ctx, e->url));
        JS_SetPropertyStr(ctx, ep, "source", JS_NewString(ctx, "ast_analysis"));
        if (e->body) JS_SetPropertyStr(ctx, ep, "body", JS_NewString(ctx, e->body));
        JSValue params = JS_NewArray(ctx);
        for (int j = 0; j < e->params_n; j++) { EpParam *p = &e->params[j];
            JSValue po = JS_NewObject(ctx);
            JS_SetPropertyStr(ctx, po, "name", JS_NewString(ctx, p->name ? p->name : ""));
            JS_SetPropertyStr(ctx, po, "location", JS_NewString(ctx, p->location ? p->location : "query"));
            JSValue vv = JS_NewArray(ctx);
            for (int k = 0; k < p->vals_n; k++) JS_SetPropertyUint32(ctx, vv, (uint32_t)k, JS_NewString(ctx, p->vals[k]));
            JS_SetPropertyStr(ctx, po, "validValues", vv);
            JS_SetPropertyUint32(ctx, params, (uint32_t)j, po);
        }
        JS_SetPropertyStr(ctx, ep, "params", params);
        if (e->headers_n) {
            JSValue hobj = JS_NewObject(ctx);
            for (int j = 0; j < e->headers_n; j++) JS_SetPropertyStr(ctx, hobj, e->headers[j].name, JS_NewString(ctx, e->headers[j].value));
            JS_SetPropertyStr(ctx, ep, "headers", hobj);
        }
        JS_SetPropertyUint32(ctx, arr, oi++, ep);
    }
    for (int i = 0; i < N; i++) mep_free1(&m[i]);
    free(m);
    return arr;
}

static void endpoint_reset(void) {
    for (int i = 0; i < g_eps_n; i++) {
        Endpoint *e = &g_eps[i];
        free(e->method); free(e->url); free(e->source); free(e->body);
        for (int j = 0; j < e->params_n; j++) { free(e->params[j].name); free(e->params[j].location); for (int k = 0; k < e->params[j].vals_n; k++) free(e->params[j].vals[k]); free(e->params[j].vals); }
        free(e->params);
        for (int j = 0; j < e->headers_n; j++) { free(e->headers[j].name); free(e->headers[j].value); }
        free(e->headers);
    }
    free(g_eps); g_eps = NULL; g_eps_n = 0; g_eps_cap = 0;
}

void endpoint_init(JSContext *ctx) { (void)ctx; endpoint_reset(); }
void endpoint_free(JSContext *ctx) { (void)ctx; endpoint_reset(); }

/* Capture request header name:value pairs into ep.headers (required-headers replay spec). Reads a plain object
   OR a `new Headers()`'s __fields, and a concolic value's EXAMPLE (`'Bearer '+token` -> the real token). ONE
   home for fetch + XHR (no duplication) — the header side of building an @H endpoint record, so it lives with
   the endpoint sink, not the engine entry. */
void capture_headers(JSContext *ctx, JSValueConst ep, JSValueConst hdrs) {
    if (!JS_IsObject(hdrs) || JS_IsConcolic(hdrs)) return;
    JSValue hf = JS_GetPropertyStr(ctx, hdrs, "__fields");
    JSValueConst hsrc = JS_IsObject(hf) ? (JSValueConst)hf : hdrs;
    JSValue hobj = JS_NewObject(ctx); int any = 0;
    JSPropertyEnum *tab = NULL; uint32_t hn = 0;
    if (JS_GetOwnPropertyNames(ctx, &tab, &hn, hsrc, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) == 0) {
        for (uint32_t hi = 0; hi < hn; hi++) {
            const char *hk = JS_AtomToCString(ctx, tab[hi].atom);
            JSValue hv = JS_GetProperty(ctx, hsrc, tab[hi].atom);
            JSValue hex = JS_IsConcolic(hv) ? JS_ConcolicExample(ctx, hv) : JS_UNDEFINED;
            const char *hvs = !JS_IsUndefined(hex) ? JS_ToCString(ctx, hex) : JS_ToCString(ctx, hv);
            JS_FreeValue(ctx, hex);
            if (hk && hvs) { JS_SetPropertyStr(ctx, hobj, hk, JS_NewString(ctx, hvs)); any = 1; }
            if (hk) JS_FreeCString(ctx, hk);
            if (hvs) JS_FreeCString(ctx, hvs);
            JS_FreeValue(ctx, hv);
        }
        JS_FreePropertyEnum(ctx, tab, hn);
    }
    JS_FreeValue(ctx, hf);
    if (any) JS_SetPropertyStr(ctx, ep, "headers", hobj); else JS_FreeValue(ctx, hobj);
}
