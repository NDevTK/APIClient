/* @S solver — see solve.h. Forced-exec candidate: derive the breakout from the sink's lexical CONTEXT, inject
   it at the source, re-run the REAL code, and verify it FIRES. */
#include "solver/solve.h"
#include "solver/concolic.h"
#include "check.h"
#include <lexbor/html/html.h>
#include <lexbor/dom/dom.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

enum { SINK_EVAL = 0, SINK_HTML = 1, SINK_URL = 2 };   /* JS / HTML / URL context -> different candidate set + fire oracle */

static int g_fired = 0;      /* set by the X9 marker when a constructed PoC executes */
static int g_verifying = 0;  /* 1 during a candidate re-run: the sink executes/re-parses the (now concrete) arg */

typedef struct { char *src; int sink; } Cand;   /* a detected sink awaiting fire-verification */
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
static const char *CANDS_JS[] = {
    "X9()",           /* statement / expression position — the input runs directly */
    "';X9();//",      /* single-quoted string context */
    "\";X9();//",     /* double-quoted string context */
    "`;X9();//",      /* template-literal context */
    "${X9()}",        /* template interpolation */
    NULL
};
static const char *CANDS_HTML[] = {
    "<svg onload=X9()>",           /* HTML-text context: an auto-firing element */
    "<img src=x onerror=X9()>",    /* HTML-text: img onerror fires on the bad src */
    "\"><svg onload=X9()>",        /* break out of a double-quoted attribute, then a firing element */
    "'><svg onload=X9()>",         /* single-quoted attribute */
    "></textarea><svg onload=X9()>", /* rawtext element (textarea/title/...) — close it first */
    NULL
};
static const char *CANDS_URL[] = {
    "javascript:X9()",     /* URL context: the vector IS the javascript: scheme (one fixed context) */
    "javascript:X9()//",
    NULL
};
static const char **cand_set(int sink) {
    return sink == SINK_HTML ? CANDS_HTML : sink == SINK_URL ? CANDS_URL : CANDS_JS;
}

static void add_pending(const char *src, int sink) {
    for (int i = 0; i < g_pending_n; i++) if (g_pending[i].sink == sink && !strcmp(g_pending[i].src, src)) return;   /* dedup */
    if (g_pending_n >= g_pending_cap) { g_pending_cap = g_pending_cap ? g_pending_cap * 2 : 8; g_pending = realloc(g_pending, (size_t)g_pending_cap * sizeof(Cand)); CHECK(g_pending, "solve: OOM pending"); }
    g_pending[g_pending_n].src = strdup(src); g_pending[g_pending_n].sink = sink; g_pending_n++;
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
    add_pending(src ? src : (shape ? shape : "?"), SINK_EVAL);   /* record the source; breakout SEARCHED at verify */
}

/* HTML firing oracle: re-parse the sink output with the REAL Lexbor parser and FIRE the auto-firing event
   handlers (svg/body onload, img/script onerror) by eval'ing their JS — X9 fires iff a breakout placed
   executable JS in an auto-firing position. innerHTML does NOT run <script>, so those never fire (correct). */
static void html_fire_walk(JSContext *ctx, lxb_dom_node_t *node) {
    for (lxb_dom_node_t *n = node; n; n = n->next) {
        if (n->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            lxb_dom_element_t *el = lxb_dom_interface_element(n);
            static const char *H[] = { "onload", "onerror", NULL };   /* AUTO-firing only (onmouseover needs interaction) */
            for (int h = 0; H[h]; h++) {
                size_t vl = 0;
                const lxb_char_t *v = lxb_dom_element_get_attribute(el, (const lxb_char_t *)H[h], strlen(H[h]), &vl);
                if (v && vl) { JSValue r = JS_Eval(ctx, (const char *)v, vl, "<handler>", JS_EVAL_TYPE_GLOBAL); JS_FreeValue(ctx, r); }
            }
        }
        if (n->first_child) html_fire_walk(ctx, n->first_child);
    }
}
static void html_fire(JSContext *ctx, const char *html) {
    lxb_html_document_t *doc = lxb_html_document_create();
    if (!doc) return;
    if (lxb_html_document_parse(doc, (const lxb_char_t *)html, strlen(html)) == LXB_STATUS_OK) {
        lxb_dom_element_t *root = lxb_dom_document_element(&doc->dom_document);
        if (root) html_fire_walk(ctx, lxb_dom_interface_node(root));
    }
    lxb_html_document_destroy(doc);
}

/* URL firing oracle: navigating to a `javascript:` URL executes its JS. So the "fire" is: if the URL scheme is
   javascript:, eval the part after the colon — X9 fires iff the breakout made the URL a javascript: one. */
static void url_fire(JSContext *ctx, const char *url) {
    while (*url == ' ' || *url == '\t' || *url == '\n') url++;   /* leading whitespace is ignored by the URL parser */
    if (!strncasecmp(url, "javascript:", 11)) {
        const char *js = url + 11;
        JSValue r = JS_Eval(ctx, js, strlen(js), "<js-url>", JS_EVAL_TYPE_GLOBAL); JS_FreeValue(ctx, r);
    }
}
/* location = arg (or el.href = arg): a URL-context sink. */
void solve_url_sink(JSContext *ctx, JSValueConst arg) {
    if (g_verifying) {
        if (concolic_is(arg)) return;
        const char *url = JS_ToCString(ctx, arg);
        if (url) { url_fire(ctx, url); JS_FreeCString(ctx, url); }
        return;
    }
    if (!concolic_is(arg)) return;
    const char *shape = concolic_shape_c(arg);
    const char *src = concolic_src_c(arg);
    add_pending(src ? src : (shape ? shape : "?"), SINK_URL);
}

/* innerHTML = arg: an HTML-context sink. Detection records the source; the candidate run re-parses the injected
   HTML and fires its handlers. */
void solve_html_sink(JSContext *ctx, JSValueConst arg) {
    if (g_verifying) {
        if (concolic_is(arg)) return;   /* injection didn't reach this write */
        const char *html = JS_ToCString(ctx, arg);
        if (html) { html_fire(ctx, html); JS_FreeCString(ctx, html); }
        return;
    }
    if (!concolic_is(arg)) return;
    const char *shape = concolic_shape_c(arg);
    const char *src = concolic_src_c(arg);
    add_pending(src ? src : (shape ? shape : "?"), SINK_HTML);
}

/* Fire-verify every pending source: SEARCH the candidate breakouts — inject each at the source, re-run the
   REAL program, and the FIRST that makes X9 fire is the replay-verified PoC (re-execution is the oracle, so no
   static context detection is needed). `rerun(ctx, ud)` re-executes the page (boot). */
void solve_verify(JSContext *ctx, void (*rerun)(JSContext *ctx, void *ud), void *ud) {
    for (int i = 0; i < g_pending_n; i++) {
        const char **cands = cand_set(g_pending[i].sink);
        const char *sink_name = g_pending[i].sink == SINK_HTML ? "innerHTML" : g_pending[i].sink == SINK_URL ? "location" : "eval";
        for (int c = 0; cands[c]; c++) {
            concolic_set_candidate(g_pending[i].src, cands[c]);
            g_fired = 0; g_verifying = 1;
            rerun(ctx, ud);
            g_verifying = 0;
            concolic_set_candidate(NULL, NULL);
            if (g_fired) { record_sink(sink_name, g_pending[i].src, cands[c]); break; }   /* this breakout fired -> the PoC */
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
    for (int i = 0; i < g_pending_n; i++) free(g_pending[i].src);
    free(g_pending); g_pending = NULL; g_pending_n = g_pending_cap = 0;
    for (int i = 0; i < g_sinks_n; i++) { free(g_sinks[i].sink); free(g_sinks[i].source); free(g_sinks[i].poc); }
    free(g_sinks); g_sinks = NULL; g_sinks_n = g_sinks_cap = 0;
}
