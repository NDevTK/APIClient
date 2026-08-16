/* @H endpoint surface — see endpoint.h. Findings are C data; params + values merge in C; emit is C. */
#include "solver/endpoint.h"
#include "core/json_buf.h"
#include "solver/concolic.h"
#include "solver/flow.h"
#include "check.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

typedef struct { char *name; char **vals; int nvals, vcap; } Param;   /* validValues merged across same-shape hits */
typedef struct { char *name; char *value; } EpHeader;   /* the transport half: what the request must carry */
typedef struct { char *method; char *path; Param *params; int np, pcap;
                 EpHeader *hdrs; int nh, hcap; } Endpoint;

/* A value carrying a `{hole}` is a SHAPE — an unknown the code did not compute — and a hole-free one is the
   real thing. The distinction decides the merge: a concrete value supersedes a shape for the same header, which
   is exactly what a param's example values already do. */
static int header_is_shape(const char *v) { return strchr(v, '{') != NULL; }

/* Returns HOW MANY HEADER NAMES THIS ENDPOINT DID NOT HAVE BEFORE — the caller credits the WFQ with it, and
   that is the whole reason this is not `void` any more. See the merge site. */
static int endpoint_merge_headers(Endpoint *e, const EndpointHeader *hdrs, int nhdrs) {
    int gained = 0;
    for (int i = 0; i < nhdrs; i++) {
        const char *n = hdrs[i].name, *v = hdrs[i].value ? hdrs[i].value : "";
        int j, found = 0;
        if (!n || !n[0]) continue;
        for (j = 0; j < e->nh; j++) {
            if (strcmp(e->hdrs[j].name, n)) continue;
            found = 1;
            if (header_is_shape(e->hdrs[j].value) && !header_is_shape(v)) {
                free(e->hdrs[j].value);
                e->hdrs[j].value = strdup(v);
                CHECK(e->hdrs[j].value, "endpoint: OOM refining a header value");
            }
            break;
        }
        if (found) continue;
        if (e->nh >= e->hcap) {
            e->hcap = e->hcap ? e->hcap * 2 : 4;
            e->hdrs = realloc(e->hdrs, (size_t)e->hcap * sizeof(EpHeader));
            CHECK(e->hdrs, "endpoint: OOM growing an endpoint's headers");
        }
        e->hdrs[e->nh].name = strdup(n);
        e->hdrs[e->nh].value = strdup(v);
        CHECK(e->hdrs[e->nh].name && e->hdrs[e->nh].value, "endpoint: OOM copying a header");
        e->nh++;
        gained++;
    }
    return gained;
}

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

void endpoint_record(JSContext *ctx, const char *method, JSValueConst url,
                     const EndpointHeader *hdrs, int nhdrs) {
    if (g_suppress) return;   /* candidate/verify run -> not a real @H endpoint */
    char *disp = url_display(ctx, url);
    char *path; KV *kv; int n;
    parse_url(disp, &path, &kv, &n);
    free(disp);

    for (int i = 0; i < g_eps_n; i++) {                 /* merge into an existing same-identity endpoint */
        if (same_identity(&g_eps[i], method, path, kv, n)) {
            for (int j = 0; j < n; j++) param_add_val(&g_eps[i].params[j], kv[j].val);
            /* A REQUIRED HEADER THIS ENDPOINT DID NOT HAVE IS EMITTED OUTPUT, and this path credited the WFQ
               with nothing for it. An endpoint's IDENTITY is method + path + param NAMES (same_identity), so a
               flow that builds a differently-SHAPED request already earns its point below — what reached here
               and went uncounted was a flow that called a known endpoint and revealed that it also wants
               `Authorization`, or a content type, or an API key. §What-the-tool-produces names required headers
               as one of the things this engine exists to learn, so a flow that learns one has emitted, and the
               ranking has to see it or the arm that found the authenticated call sinks back among arms that
               found nothing.
               STRUCTURE, NOT DATA — the line this credit stops at, and the reason `param_add_val` above earns
               nothing. A header NAME is a fact about what the endpoint requires, bounded by the code; a param
               VALUE is a better example of something already known, and it is unbounded in the INPUT, so a loop
               over opaque data could mint credit without limit and outrank the whole frontier by generating
               strings. One point per merge that gained structure, matching the granularity below: an endpoint
               is one discovery however many headers arrive with it, and a header the surface gains later is
               one more. */
            if (endpoint_merge_headers(&g_eps[i], hdrs, nhdrs) > 0)
                flow_credit_emit(1.0);
            goto done;
        }
    }
    if (g_eps_n >= g_eps_cap) { g_eps_cap = g_eps_cap ? g_eps_cap * 2 : 16; g_eps = realloc(g_eps, (size_t)g_eps_cap * sizeof(Endpoint)); CHECK(g_eps, "endpoint: OOM surface"); }
    Endpoint *e = &g_eps[g_eps_n++];
    memset(e, 0, sizeof *e);
    e->method = strdup(method); e->path = strdup(path);
    if (n) { e->params = calloc(n, sizeof(Param)); CHECK(e->params, "endpoint: OOM params"); }
    for (int j = 0; j < n; j++) { e->params[e->np].name = strdup(kv[j].name); param_add_val(&e->params[e->np], kv[j].val); e->np++; }
    /* The count is deliberately DROPPED here: every header of a brand-new endpoint is new, and the discovery
       being credited is the ENDPOINT. Crediting both would price one sighting at one point plus one per header
       it happened to carry, which makes a request's header count part of the ranking. */
    (void)endpoint_merge_headers(e, hdrs, nhdrs);
    flow_credit_emit(1.0);   /* a NEW endpoint: this flow just emitted value-of-information -> WFQ reward */
done:
    free(path);
    for (int j = 0; j < n; j++) { free(kv[j].name); free(kv[j].val); }
    free(kv);
}

/* Serialize the @H surface DIRECTLY to a JSON string in C (caller frees) — no JS-object round-trip. The
   writer is core/json_buf.h's: this file and solve.c each carried a private copy of it, which is one copy too
   many of a thing that has exactly one correct behaviour. */
char *endpoint_json_array(void) {
    JsonBuf b = { 0 };
    json_buf_puts(&b, "[");
    for (int i = 0; i < g_eps_n; i++) {
        Endpoint *e = &g_eps[i];
        if (i) json_buf_puts(&b, ",");
        json_buf_puts(&b, "{\"method\":"); json_buf_str(&b, e->method);
        json_buf_puts(&b, ",\"url\":"); json_buf_str(&b, e->path);
        json_buf_puts(&b, ",\"params\":[");
        for (int j = 0; j < e->np; j++) {
            if (j) json_buf_puts(&b, ",");
            json_buf_puts(&b, "{\"name\":"); json_buf_str(&b, e->params[j].name);
            json_buf_puts(&b, ",\"validValues\":[");
            for (int k = 0; k < e->params[j].nvals; k++) { if (k) json_buf_puts(&b, ","); json_buf_str(&b, e->params[j].vals[k]); }
            json_buf_puts(&b, "]}");
        }
        json_buf_puts(&b, "]");
        /* The transport half, and ONLY when there is one — an endpoint with no learned header must not claim an
           empty requirement, which reads as "needs nothing" rather than "nothing was observed". A record keyed
           by header name, which is the shape the popup's Required Headers section already reads. */
        if (e->nh) {
            json_buf_puts(&b, ",\"headers\":{");
            for (int j = 0; j < e->nh; j++) {
                if (j) json_buf_puts(&b, ",");
                json_buf_str(&b, e->hdrs[j].name);
                json_buf_puts(&b, ":");
                json_buf_str(&b, e->hdrs[j].value);
            }
            json_buf_puts(&b, "}");
        }
        json_buf_puts(&b, "}");
    }
    json_buf_puts(&b, "]");
    return json_buf_take(&b);
}

int endpoint_count(void) { return g_eps_n; }

void endpoint_free(void) {
    for (int i = 0; i < g_eps_n; i++) {
        free(g_eps[i].method); free(g_eps[i].path);
        for (int j = 0; j < g_eps[i].np; j++) { free(g_eps[i].params[j].name); for (int k = 0; k < g_eps[i].params[j].nvals; k++) free(g_eps[i].params[j].vals[k]); free(g_eps[i].params[j].vals); }
        free(g_eps[i].params);
        for (int j = 0; j < g_eps[i].nh; j++) { free(g_eps[i].hdrs[j].name); free(g_eps[i].hdrs[j].value); }
        free(g_eps[i].hdrs);
    }
    free(g_eps); g_eps = NULL; g_eps_n = g_eps_cap = 0;
}
