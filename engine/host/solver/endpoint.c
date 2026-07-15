/* @H endpoint surface — see endpoint.h. */
#include "solver/endpoint.h"
#include "solver/concolic.h"
#include "check.h"
#include <stdlib.h>
#include <string.h>

typedef struct { char *method; char *url; } Endpoint;
static Endpoint *g_eps = NULL;
static int g_eps_n = 0, g_eps_cap = 0;

void endpoint_init(void) { g_eps = NULL; g_eps_n = 0; g_eps_cap = 0; }

static char *url_display(JSContext *ctx, JSValueConst url) {
    if (concolic_is(url)) { const char *s = concolic_shape_c(url); return strdup(s ? s : "{}"); }
    const char *s = JS_ToCString(ctx, url);
    char *r = strdup(s ? s : "?");
    if (s) JS_FreeCString(ctx, s);
    return r;
}

void endpoint_record(JSContext *ctx, const char *method, JSValueConst url) {
    char *u = url_display(ctx, url);
    for (int i = 0; i < g_eps_n; i++)                       /* dedup on (method, url) */
        if (!strcmp(g_eps[i].method, method) && !strcmp(g_eps[i].url, u)) { free(u); return; }
    if (g_eps_n >= g_eps_cap) {
        g_eps_cap = g_eps_cap ? g_eps_cap * 2 : 16;
        g_eps = realloc(g_eps, (size_t)g_eps_cap * sizeof(Endpoint));
        CHECK(g_eps, "endpoint: OOM growing the @H surface");
    }
    g_eps[g_eps_n].method = strdup(method);
    g_eps[g_eps_n].url = u;
    g_eps_n++;
}

/* Serialize the @H surface DIRECTLY to a JSON string in C. The findings are C data, so their serialization is
   C too — never a JS-object round-trip (that creates JS garbage AND re-implements JSON.stringify the engine
   already owns for real values). Returns a malloc'd string { "fetchCallSites":[ {"method":..,"url":..}, .. ] };
   the caller frees. */
typedef struct { char *b; size_t n, cap; } Buf;
static void buf_ensure(Buf *b, size_t extra) {
    if (b->n + extra + 1 <= b->cap) return;
    while (b->n + extra + 1 > b->cap) b->cap = b->cap ? b->cap * 2 : 256;
    b->b = realloc(b->b, b->cap); CHECK(b->b, "endpoint: OOM building @H JSON");
}
static void buf_puts(Buf *b, const char *s) { size_t l = strlen(s); buf_ensure(b, l); memcpy(b->b + b->n, s, l); b->n += l; }
static void buf_json_str(Buf *b, const char *s) {   /* a JSON string literal with the minimal required escapes */
    buf_ensure(b, 2); b->b[b->n++] = '"';
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
char *endpoint_json(void) {
    Buf b = { 0 };
    buf_puts(&b, "{\"fetchCallSites\":[");
    for (int i = 0; i < g_eps_n; i++) {
        if (i) buf_puts(&b, ",");
        buf_puts(&b, "{\"method\":"); buf_json_str(&b, g_eps[i].method);
        buf_puts(&b, ",\"url\":"); buf_json_str(&b, g_eps[i].url);
        buf_puts(&b, "}");
    }
    buf_puts(&b, "]}");
    buf_ensure(&b, 1); b.b[b.n] = 0;
    return b.b;
}

int endpoint_count(void) { return g_eps_n; }

void endpoint_free(void) {
    for (int i = 0; i < g_eps_n; i++) { free(g_eps[i].method); free(g_eps[i].url); }
    free(g_eps); g_eps = NULL; g_eps_n = g_eps_cap = 0;
}
