/* @H endpoint surface — see endpoint.h. Findings are C data; params + values merge in C; emit is C. */
#include "solver/endpoint.h"
#include "solver/concolic.h"
#include "solver/flow.h"
#include "check.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

typedef struct { char *name; char **vals; int nvals, vcap; } Param;   /* validValues merged across same-shape hits */
typedef struct { char *method; char *path; Param *params; int np, pcap; } Endpoint;

static Endpoint *g_eps = NULL;
static int g_eps_n = 0, g_eps_cap = 0;
static int g_suppress = 0;   /* a candidate/verify re-run's requests are @S artifacts, NOT real @H */

void endpoint_init(void) { g_eps = NULL; g_eps_n = 0; g_eps_cap = 0; g_suppress = 0; }
void endpoint_suppress(int on) { g_suppress = on ? 1 : 0; }

static char *url_display(JSContext *ctx, JSValueConst url) {
    if (concolic_is(url)) { const char *s = concolic_shape_c(url); return strdup(s ? s : "{}"); }
    const char *s = JS_ToCString(ctx, url);
    char *r = strdup(s ? s : "?");
    if (s) JS_FreeCString(ctx, s);
    return r;
}

/* a parsed URL: path + query params (name -> value, in query order) */
typedef struct { char *name; char *val; } KV;
static void parse_url(const char *disp, char **out_path, KV **out_kv, int *out_n) {
    const char *q = strchr(disp, '?');
    size_t plen = q ? (size_t)(q - disp) : strlen(disp);
    char *path = malloc(plen + 1); CHECK(path, "endpoint: OOM path"); memcpy(path, disp, plen); path[plen] = 0;
    KV *kv = NULL; int n = 0, cap = 0;
    if (q && q[1]) {
        char *dup = strdup(q + 1); CHECK(dup, "endpoint: OOM query");
        for (char *tok = strtok(dup, "&"); tok; tok = strtok(NULL, "&")) {
            char *eq = strchr(tok, '=');
            const char *name = tok, *val = "";
            if (eq) { *eq = 0; val = eq + 1; }
            if (n >= cap) { cap = cap ? cap * 2 : 8; kv = realloc(kv, (size_t)cap * sizeof(KV)); CHECK(kv, "endpoint: OOM kv"); }
            kv[n].name = strdup(name); kv[n].val = strdup(val); n++;
        }
        free(dup);
    }
    *out_path = path; *out_kv = kv; *out_n = n;
}

static void param_add_val(Param *p, const char *v) {   /* merge a validValue (dedup, skip empty) */
    if (!v || !v[0]) return;
    for (int i = 0; i < p->nvals; i++) if (!strcmp(p->vals[i], v)) return;
    if (p->nvals >= p->vcap) { p->vcap = p->vcap ? p->vcap * 2 : 4; p->vals = realloc(p->vals, (size_t)p->vcap * sizeof(char *)); CHECK(p->vals, "endpoint: OOM vals"); }
    p->vals[p->nvals++] = strdup(v);
}

/* an endpoint's IDENTITY is (method, path, param-name-set) — same identity merges param values. */
static int same_identity(Endpoint *e, const char *method, const char *path, KV *kv, int n) {
    if (strcmp(e->method, method) || strcmp(e->path, path) || e->np != n) return 0;
    for (int i = 0; i < n; i++) if (strcmp(e->params[i].name, kv[i].name)) return 0;
    return 1;
}

void endpoint_record(JSContext *ctx, const char *method, JSValueConst url) {
    if (g_suppress) return;   /* candidate/verify run -> not a real @H endpoint */
    char *disp = url_display(ctx, url);
    char *path; KV *kv; int n;
    parse_url(disp, &path, &kv, &n);
    free(disp);

    for (int i = 0; i < g_eps_n; i++) {                 /* merge into an existing same-identity endpoint */
        if (same_identity(&g_eps[i], method, path, kv, n)) {
            for (int j = 0; j < n; j++) param_add_val(&g_eps[i].params[j], kv[j].val);
            goto done;
        }
    }
    if (g_eps_n >= g_eps_cap) { g_eps_cap = g_eps_cap ? g_eps_cap * 2 : 16; g_eps = realloc(g_eps, (size_t)g_eps_cap * sizeof(Endpoint)); CHECK(g_eps, "endpoint: OOM surface"); }
    Endpoint *e = &g_eps[g_eps_n++];
    memset(e, 0, sizeof *e);
    e->method = strdup(method); e->path = strdup(path);
    if (n) { e->params = calloc(n, sizeof(Param)); CHECK(e->params, "endpoint: OOM params"); }
    for (int j = 0; j < n; j++) { e->params[e->np].name = strdup(kv[j].name); param_add_val(&e->params[e->np], kv[j].val); e->np++; }
    flow_credit_emit(1.0);   /* a NEW endpoint: this flow just emitted value-of-information -> WFQ reward */
done:
    free(path);
    for (int j = 0; j < n; j++) { free(kv[j].name); free(kv[j].val); }
    free(kv);
}

/* Serialize the @H surface DIRECTLY to a JSON string in C (caller frees) — no JS-object round-trip. */
typedef struct { char *b; size_t n, cap; } Buf;
static void buf_ensure(Buf *b, size_t extra) {
    if (b->n + extra + 1 <= b->cap) return;
    while (b->n + extra + 1 > b->cap) b->cap = b->cap ? b->cap * 2 : 256;
    b->b = realloc(b->b, b->cap); CHECK(b->b, "endpoint: OOM JSON");
}
static void buf_puts(Buf *b, const char *s) { size_t l = strlen(s); buf_ensure(b, l); memcpy(b->b + b->n, s, l); b->n += l; }
static void buf_json_str(Buf *b, const char *s) {
    buf_ensure(b, 1); b->b[b->n++] = '"';
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { buf_ensure(b, 2); b->b[b->n++] = '\\'; b->b[b->n++] = (char)c; }
        else if (c == '\n') buf_puts(b, "\\n");
        else if (c == '\r') buf_puts(b, "\\r");
        else if (c == '\t') buf_puts(b, "\\t");
        else if (c < 0x20) { char t[8]; snprintf(t, sizeof t, "\\u%04x", c); buf_puts(b, t); }
        else { buf_ensure(b, 1); b->b[b->n++] = (char)c; }
    }
    buf_ensure(b, 1); b->b[b->n++] = '"';
}
char *endpoint_json_array(void) {
    Buf b = { 0 };
    buf_puts(&b, "[");
    for (int i = 0; i < g_eps_n; i++) {
        Endpoint *e = &g_eps[i];
        if (i) buf_puts(&b, ",");
        buf_puts(&b, "{\"method\":"); buf_json_str(&b, e->method);
        buf_puts(&b, ",\"url\":"); buf_json_str(&b, e->path);
        buf_puts(&b, ",\"params\":[");
        for (int j = 0; j < e->np; j++) {
            if (j) buf_puts(&b, ",");
            buf_puts(&b, "{\"name\":"); buf_json_str(&b, e->params[j].name);
            buf_puts(&b, ",\"validValues\":[");
            for (int k = 0; k < e->params[j].nvals; k++) { if (k) buf_puts(&b, ","); buf_json_str(&b, e->params[j].vals[k]); }
            buf_puts(&b, "]}");
        }
        buf_puts(&b, "]}");
    }
    buf_puts(&b, "]");
    buf_ensure(&b, 1); b.b[b.n] = 0;
    return b.b;
}

int endpoint_count(void) { return g_eps_n; }

void endpoint_free(void) {
    for (int i = 0; i < g_eps_n; i++) {
        free(g_eps[i].method); free(g_eps[i].path);
        for (int j = 0; j < g_eps[i].np; j++) { free(g_eps[i].params[j].name); for (int k = 0; k < g_eps[i].params[j].nvals; k++) free(g_eps[i].params[j].vals[k]); free(g_eps[i].params[j].vals); }
        free(g_eps[i].params);
    }
    free(g_eps); g_eps = NULL; g_eps_n = g_eps_cap = 0;
}
