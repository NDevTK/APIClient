/* @S solver — see solve.h. Forced-exec candidate: derive the breakout from the sink's lexical CONTEXT, inject
   it at the source, re-run the REAL code, and verify it FIRES. */
#include "solver/solve.h"
#include "solver/concolic.h"
#include "check.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static int g_fired = 0;      /* set by the X9 marker when a constructed PoC executes */
static int g_verifying = 0;  /* 1 during a candidate re-run: the eval sink executes the (now concrete) arg */

typedef struct { char *src; char *breakout; } Cand;     /* a detected sink awaiting fire-verification */
static Cand *g_pending = NULL; static int g_pending_n = 0, g_pending_cap = 0;

typedef struct { char *sink; char *source; char *poc; } Finding;   /* verified PoCs only */
static Finding *g_sinks = NULL; static int g_sinks_n = 0, g_sinks_cap = 0;

static JSValue js_x9(JSContext *ctx, JSValueConst t, int c, JSValueConst *v) { (void)ctx; (void)t; (void)c; (void)v; g_fired = 1; return JS_UNDEFINED; }

void solve_init(JSContext *ctx) {
    g_pending = NULL; g_pending_n = g_pending_cap = 0;
    g_sinks = NULL; g_sinks_n = g_sinks_cap = 0;
    g_fired = g_verifying = 0;
    JSValue g = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, g, "X9", JS_NewCFunction(ctx, js_x9, "X9", 0));
    JS_FreeValue(ctx, g);
}

/* Breakout CANDIDATES — one per common JS context. We do NOT statically detect which context applies (that
   needs a JS lexer + loses to filters/encoders the code may apply); instead we try each through the REAL code
   and the one that FIRES is the verified PoC. Re-execution is the oracle, so this survives any quoting/filter
   the code imposes and needs no lexer. */
static const char *CANDS[] = {
    "X9()",           /* statement / expression position — the input runs directly */
    "';X9();//",      /* single-quoted string context */
    "\";X9();//",     /* double-quoted string context */
    "`;X9();//",      /* template-literal context */
    "${X9()}",        /* template interpolation */
    NULL
};

static void add_pending(const char *src) {
    for (int i = 0; i < g_pending_n; i++) if (!strcmp(g_pending[i].src, src)) return;   /* dedup by source */
    if (g_pending_n >= g_pending_cap) { g_pending_cap = g_pending_cap ? g_pending_cap * 2 : 8; g_pending = realloc(g_pending, (size_t)g_pending_cap * sizeof(Cand)); CHECK(g_pending, "solve: OOM pending"); }
    g_pending[g_pending_n].src = strdup(src); g_pending[g_pending_n].breakout = NULL; g_pending_n++;
}

static void record_sink(const char *sink, const char *source, const char *poc) {
    for (int i = 0; i < g_sinks_n; i++) if (!strcmp(g_sinks[i].sink, sink) && !strcmp(g_sinks[i].source, source)) return;
    if (g_sinks_n >= g_sinks_cap) { g_sinks_cap = g_sinks_cap ? g_sinks_cap * 2 : 8; g_sinks = realloc(g_sinks, (size_t)g_sinks_cap * sizeof(Finding)); CHECK(g_sinks, "solve: OOM @S store"); }
    Finding *f = &g_sinks[g_sinks_n++];
    f->sink = strdup(sink); f->source = strdup(source ? source : "?"); f->poc = strdup(poc);
}

void solve_eval_sink(JSContext *ctx, JSValueConst arg) {
    if (g_verifying) {                          /* candidate run: the arg is the injected+wrapped CONCRETE code */
        if (concolic_is(arg)) return;           /* injection didn't reach this read -> not our candidate */
        const char *code = JS_ToCString(ctx, arg);
        if (code) { JSValue r = JS_Eval(ctx, code, strlen(code), "<poc>", JS_EVAL_TYPE_GLOBAL); JS_FreeValue(ctx, r); JS_FreeCString(ctx, code); }
        return;                                 /* g_fired now reflects whether the PoC executed */
    }
    if (!concolic_is(arg)) return;              /* detection: not an attacker source -> not a sink */
    const char *shape = concolic_shape_c(arg);
    const char *src = concolic_src_c(arg);
    add_pending(src ? src : (shape ? shape : "?"));   /* record the source; the breakout is SEARCHED at verify */
}

/* Fire-verify every pending source: SEARCH the candidate breakouts — inject each at the source, re-run the
   REAL program, and the FIRST that makes X9 fire is the replay-verified PoC (re-execution is the oracle, so no
   static context detection is needed). `rerun(ctx, ud)` re-executes the page (boot). */
void solve_verify(JSContext *ctx, void (*rerun)(JSContext *ctx, void *ud), void *ud) {
    for (int i = 0; i < g_pending_n; i++) {
        for (int c = 0; CANDS[c]; c++) {
            concolic_set_candidate(g_pending[i].src, CANDS[c]);
            g_fired = 0; g_verifying = 1;
            rerun(ctx, ud);
            g_verifying = 0;
            concolic_set_candidate(NULL, NULL);
            if (g_fired) { record_sink("eval", g_pending[i].src, CANDS[c]); break; }   /* this breakout fired -> the PoC */
        }
    }
}

/* ── @S JSON emit (C-native) ── */
typedef struct { char *b; size_t n, cap; } Buf;
static void buf_ensure(Buf *b, size_t extra) { if (b->n + extra + 1 <= b->cap) return; while (b->n + extra + 1 > b->cap) b->cap = b->cap ? b->cap * 2 : 256; b->b = realloc(b->b, b->cap); CHECK(b->b, "solve: OOM JSON"); }
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
    for (int i = 0; i < g_pending_n; i++) { free(g_pending[i].src); free(g_pending[i].breakout); }
    free(g_pending); g_pending = NULL; g_pending_n = g_pending_cap = 0;
    for (int i = 0; i < g_sinks_n; i++) { free(g_sinks[i].sink); free(g_sinks[i].source); free(g_sinks[i].poc); }
    free(g_sinks); g_sinks = NULL; g_sinks_n = g_sinks_cap = 0;
}
