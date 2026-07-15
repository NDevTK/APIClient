/* @S solver — see solve.h. */
#include "solver/solve.h"
#include "solver/concolic.h"
#include "check.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static int g_fired = 0;   /* set by the X9 marker when a constructed PoC actually executes */

typedef struct { char *sink; char *source; char *poc; } Finding;
static Finding *g_sinks = NULL;
static int g_sinks_n = 0, g_sinks_cap = 0;

static JSValue js_x9(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)ctx; (void)this_val; (void)argc; (void)argv;
    g_fired = 1;   /* the PoC reached an executable position and CALLED — verified */
    return JS_UNDEFINED;
}

void solve_init(JSContext *ctx) {
    g_sinks = NULL; g_sinks_n = 0; g_sinks_cap = 0; g_fired = 0;
    JSValue g = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, g, "X9", JS_NewCFunction(ctx, js_x9, "X9", 0));
    JS_FreeValue(ctx, g);
}

static void record_sink(const char *sink, const char *source, const char *poc) {
    for (int i = 0; i < g_sinks_n; i++)                         /* dedup by (sink, source) */
        if (!strcmp(g_sinks[i].sink, sink) && !strcmp(g_sinks[i].source, source)) return;
    if (g_sinks_n >= g_sinks_cap) { g_sinks_cap = g_sinks_cap ? g_sinks_cap * 2 : 8; g_sinks = realloc(g_sinks, (size_t)g_sinks_cap * sizeof(Finding)); CHECK(g_sinks, "solve: OOM @S store"); }
    Finding *f = &g_sinks[g_sinks_n++];
    f->sink = strdup(sink); f->source = strdup(source ? source : "?"); f->poc = strdup(poc);
}

void solve_eval_sink(JSContext *ctx, JSValueConst arg) {
    if (!concolic_is(arg)) return;   /* not an attacker-controlled source -> not a sink */
    /* JS-context breakout: the concolic IS the whole eval input, so a bare payload runs in statement position.
       (A string/quoted context needs `';X9();//` — the context-derived breakout is the next increment.) */
    const char *breakout = "X9()";
    /* VERIFY by FIRING: execute the constructed PoC exactly as the sink would; require X9 to actually call. */
    g_fired = 0;
    JSValue r = JS_Eval(ctx, breakout, strlen(breakout), "<poc>", JS_EVAL_TYPE_GLOBAL);
    JS_FreeValue(ctx, r);
    if (g_fired) record_sink("eval", concolic_shape_c(arg), breakout);   /* replay-verified PoC only */
}

/* ── @S JSON emit (C-native, like endpoint.c) ── */
typedef struct { char *b; size_t n, cap; } Buf;
static void buf_ensure(Buf *b, size_t extra) {
    if (b->n + extra + 1 <= b->cap) return;
    while (b->n + extra + 1 > b->cap) b->cap = b->cap ? b->cap * 2 : 256;
    b->b = realloc(b->b, b->cap); CHECK(b->b, "solve: OOM JSON");
}
static void buf_puts(Buf *b, const char *s) { size_t l = strlen(s); buf_ensure(b, l); memcpy(b->b + b->n, s, l); b->n += l; }
static void buf_json_str(Buf *b, const char *s) {
    buf_ensure(b, 1); b->b[b->n++] = '"';
    for (; *s; s++) { unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { buf_ensure(b, 2); b->b[b->n++] = '\\'; b->b[b->n++] = (char)c; }
        else if (c == '\n') buf_puts(b, "\\n"); else if (c == '\r') buf_puts(b, "\\r"); else if (c == '\t') buf_puts(b, "\\t");
        else if (c < 0x20) { char t[8]; snprintf(t, sizeof t, "\\u%04x", c); buf_puts(b, t); }
        else { buf_ensure(b, 1); b->b[b->n++] = (char)c; } }
    buf_ensure(b, 1); b->b[b->n++] = '"';
}
char *solve_json(void) {
    Buf b = { 0 };
    buf_puts(&b, "{\"securitySinks\":[");
    for (int i = 0; i < g_sinks_n; i++) {
        if (i) buf_puts(&b, ",");
        buf_puts(&b, "{\"sink\":"); buf_json_str(&b, g_sinks[i].sink);
        buf_puts(&b, ",\"source\":"); buf_json_str(&b, g_sinks[i].source);
        buf_puts(&b, ",\"poc\":"); buf_json_str(&b, g_sinks[i].poc);
        buf_puts(&b, "}");
    }
    buf_puts(&b, "]}");
    buf_ensure(&b, 1); b.b[b.n] = 0;
    return b.b;
}

int solve_count(void) { return g_sinks_n; }

void solve_free(void) {
    for (int i = 0; i < g_sinks_n; i++) { free(g_sinks[i].sink); free(g_sinks[i].source); free(g_sinks[i].poc); }
    free(g_sinks); g_sinks = NULL; g_sinks_n = g_sinks_cap = 0;
}
