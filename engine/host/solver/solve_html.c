/* @S HTML breakout ANALYSIS — see solve_html.h. Pure Lexbor DOM + clean-realm JS eval; no scheduler state. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "solve_html.h"
#include "check.h"   /* CHECK — OOM must crash, never return 0 (a dropped @S PoC masquerading as "no breakout") */
#include <lexbor/dom/dom.h>

static int mem_has_x9(const lxb_char_t *s, size_t n) {   /* the X9 fire-marker survives, as raw bytes (values aren't null-terminated) */
    if (!s) return 0;
    for (size_t i = 0; i + 1 < n; i++) if (s[i] == 'X' && s[i + 1] == '9') return 1;
    return 0;
}
/* FIRING breakout: an on* handler / <script> body is a PoC only if, RUN as JS, the fire-marker actually
   EXECUTES — not merely if it appears (byte-present) or the code compiles. Rewrite the marker to a call
   (X9 -> X9(), mirroring the live-verify X9 -> apiclientsink mapping) and RUN it under `with(__u){…}`, a
   universal no-op stub so app functions undefined in this clean realm don't throw (`log('')` -> no-op —
   completeness, no false-negative on real handlers), then check X9 fired. This REJECTS the two classes the
   static/compile checks accept: X9 trapped inside a STRING literal (`log('<img onerror=X9>')` — never runs)
   and a malformed handler (`onerror=X9"` — a syntax error). What only live Chrome can still judge is whether
   the DOM EVENT itself fires (empty-src onerror, interaction-only) — that stays ground truth, never softened. */
static int x9_fires(const lxb_char_t *code, size_t len) {
    if (!g_solve_ctx || !code || !len) return 0;
    char *m = malloc(len * 2 + 1); CHECK(m, "x9fires-oom: malloc failed -> a dropped @S PoC would masquerade as no-breakout");   /* X9 -> X9() grows +2 per marker */
    size_t o = 0;
    for (size_t i = 0; i < len; ) {
        if (i + 1 < len && code[i] == 'X' && code[i + 1] == '9') {
            m[o++] = 'X'; m[o++] = '9';
            if (i + 2 < len && code[i + 2] == '(') { i += 2; }   /* already X9( -> leave the call as authored */
            else { m[o++] = '('; m[o++] = ')'; i += 2; }         /* reference X9 -> a CALL X9() */
        } else { m[o++] = (char)code[i++]; }
    }
    m[o] = 0;
    size_t cap = o + 64; char *buf = malloc(cap);
    CHECK(buf, "x9fires-oom: malloc failed");
    /* `\n;` closes any trailing `//` line-comment so a `…;X9();//'` handler still runs X9() */
    int n = snprintf(buf, cap, "globalThis.__f9=0;with(globalThis.__u){%s\n;}", m);
    free(m);
    DCHECK(n >= 0 && (size_t)n < cap, "x9fires-fmt: snprintf truncated/errored -> the fixed ~42-char wrapper always fits o+64");
    JSValue cr = JS_Eval(g_solve_ctx, buf, (size_t)n, "<handler>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(cr)) { JSValue e = JS_GetException(g_solve_ctx); JS_FreeValue(g_solve_ctx, e); }
    JS_FreeValue(g_solve_ctx, cr);
    free(buf);
    JSValue fv = JS_Eval(g_solve_ctx, "globalThis.__f9", 15, "<f>", JS_EVAL_TYPE_GLOBAL);
    int fired = JS_ToBool(g_solve_ctx, fv); JS_FreeValue(g_solve_ctx, fv);
    return fired;
}
struct x9_walk { int found; int script_exec; };   /* script_exec: does THIS sink run a parsed <script>? (document.write/srcdoc yes; innerHTML/outerHTML/insertAdjacentHTML NO — HTML-parser-inserted scripts are inert) */
static lxb_status_t x9_walk_cb(lxb_dom_node_t *node, void *vctx) {
    struct x9_walk *c = vctx;
    if (node->type != LXB_DOM_NODE_TYPE_ELEMENT) return LXB_STATUS_OK;
    lxb_dom_element_t *el = lxb_dom_interface_element(node);
    size_t nl = 0; const lxb_char_t *nm = lxb_dom_element_qualified_name(el, &nl);
    if (c->script_exec && nl == 6 && nm && memcmp(nm, "script", 6) == 0) {   /* a <script> is only a breakout on a sink that EXECUTES parsed scripts */
        size_t cl = 0; lxb_char_t *ct = lxb_dom_node_text_content(lxb_dom_interface_node(node), &cl);
        if (mem_has_x9(ct, cl) && x9_fires(ct, cl)) { c->found = 1; return LXB_STATUS_STOP; }   /* VALID JS that calls the marker, not just contains it */
    }
    for (lxb_dom_attr_t *a = lxb_dom_element_first_attribute(el); a; a = lxb_dom_element_next_attribute(a)) {
        size_t al = 0; const lxb_char_t *an = lxb_dom_attr_local_name(a, &al);   /* on* event handler with X9 -> a LIVE handler (Lexbor lowercases names) */
        if (al >= 3 && an && an[0] == 'o' && an[1] == 'n') {
            size_t vl = 0; const lxb_char_t *av = lxb_dom_attr_value(a, &vl);
            if (mem_has_x9(av, vl) && x9_fires(av, vl)) { c->found = 1; return LXB_STATUS_STOP; }   /* handler must EXECUTE the marker call, not merely contain X9 */
        }
    }
    return LXB_STATUS_OK;
}
/* PARSE the sink string like a real browser (Lexbor) and check X9 reached an EXECUTABLE position: a live
   element's on* handler, or <script> content. A tag opener trapped inside an ATTRIBUTE value
   (href="<img onerror=X9>") or in TEXT (&lt;img..) is INERT -> NOT a breakout. Replaces the naive
   strstr("<img") which false-positived whenever a raw tag survived ANYWHERE, incl. attribute-trapped. */
static int solve_broke_html(const char *res, int script_exec) {
    lxb_html_document_t *doc = lxb_html_document_create();
    CHECK(doc, "solve_broke_html: document_create failed -> a dropped @S PoC would masquerade as no-breakout");
    struct x9_walk c = { 0, script_exec };
    if (lxb_html_document_parse(doc, (const lxb_char_t *)res, strlen(res)) == LXB_STATUS_OK) {
        lxb_dom_node_t *root = lxb_dom_interface_node(lxb_html_document_body_element(doc));
        if (root) lxb_dom_node_simple_walk(root, x9_walk_cb, &c);
    }
    lxb_html_document_destroy(doc);
    return c.found;
}
/* context-aware breakout: did the candidate's active payload reach an EXECUTABLE position in `res`? */
int solve_broke(const char *sc, const char *res) {
    if (!res) return 0;
    if (sc && strcmp(sc, "url") == 0) { const char *p = res; while (*p == ' ' || *p == '\t') p++; return strncmp(p, "javascript:X9", 13) == 0; }
    if (sc && strcmp(sc, "scripturl") == 0) {   /* <script src>: a javascript: URL does NOT run; the breakout is an
        ATTACKER-CONTROLLED ORIGIN (the browser fetches+executes the script from there). X9 at the host = the
        candidate reached the origin position (whole src attacker-controlled, or a `//`-prefixable hole). */
        const char *p = res; while (*p == ' ' || *p == '\t') p++;
        return strncmp(p, "//X9", 4) == 0 || strncmp(p, "https://X9", 10) == 0 || strncmp(p, "http://X9", 9) == 0; }
    if (!strstr(res, "X9")) return 0;   /* our nonce must survive */
    if (sc && strcmp(sc, "js") == 0) {
        /* eval sink: the sink VALUE is code — RUN it in the clean realm; X9() firing = real code injection
           (sound, unlike a substring match which false-positives when X9 lands inside a string literal). */
        if (!g_solve_ctx) return 0;
        JSValue r0 = JS_Eval(g_solve_ctx, "globalThis.__f9=0", 17, "<r>", JS_EVAL_TYPE_GLOBAL); JS_FreeValue(g_solve_ctx, r0);
        JSValue cr = JS_Eval(g_solve_ctx, res, strlen(res), "<sinkcode>", JS_EVAL_TYPE_GLOBAL);
        if (JS_IsException(cr)) { JSValue e = JS_GetException(g_solve_ctx); JS_FreeValue(g_solve_ctx, e); }
        JS_FreeValue(g_solve_ctx, cr);
        JSValue fv = JS_Eval(g_solve_ctx, "globalThis.__f9", 15, "<f>", JS_EVAL_TYPE_GLOBAL);
        int fired = JS_ToBool(g_solve_ctx, fv); JS_FreeValue(g_solve_ctx, fv);
        return fired;
    }
    /* html/attr: PARSE like a browser and require X9 in an EXECUTABLE position (live on* handler, or <script>
       ONLY on a script-executing sink), not merely a raw tag opener surviving somewhere (that false-positives
       when the payload is trapped in an attribute value, e.g. `<a href="<img onerror=X9>">`). A parsed <script>
       is INERT under innerHTML/outerHTML/insertAdjacentHTML — sink ctx "htmls" (document.write/srcdoc) executes it. */
    return solve_broke_html(res, sc && strcmp(sc, "htmls") == 0);
}

static int mem_has_loc(const lxb_char_t *s, size_t n) {
    if (!s) return 0; const char *L = CTX_LOC; size_t Ln = strlen(L);
    for (size_t i = 0; i + Ln <= n; i++) { size_t j = 0; while (j < Ln && s[i + j] == (lxb_char_t)L[j]) j++; if (j == Ln) return 1; }
    return 0;
}
int is_rawtext_tag(const char *t) {
    return t && (!strcmp(t, "textarea") || !strcmp(t, "title") || !strcmp(t, "style") || !strcmp(t, "xmp") ||
                 !strcmp(t, "iframe") || !strcmp(t, "noembed") || !strcmp(t, "noframes") || !strcmp(t, "script"));
}
static lxb_status_t ctx_probe_cb(lxb_dom_node_t *node, void *vctx) {
    struct ctx_probe *c = vctx;
    if (c->found) return LXB_STATUS_STOP;
    if (node->type == LXB_DOM_NODE_TYPE_ELEMENT) {   /* the locator may sit in one of THIS element's attribute values */
        lxb_dom_element_t *el = lxb_dom_interface_element(node);
        for (lxb_dom_attr_t *a = lxb_dom_element_first_attribute(el); a; a = lxb_dom_element_next_attribute(a)) {
            size_t vl = 0; const lxb_char_t *av = lxb_dom_attr_value(a, &vl);
            if (av && mem_has_loc(av, vl)) {
                size_t nl = 0; const lxb_char_t *nm = lxb_dom_element_qualified_name(el, &nl);
                if (nm && nl > 0 && nl < sizeof(c->tag)) { memcpy(c->tag, nm, nl); c->tag[nl] = 0; }
                c->is_attr = 1; c->found = 1; return LXB_STATUS_STOP;
            }
        }
        return LXB_STATUS_OK;
    }
    if (node->type != LXB_DOM_NODE_TYPE_TEXT && node->type != LXB_DOM_NODE_TYPE_COMMENT) return LXB_STATUS_OK;
    size_t l = 0; lxb_char_t *t = lxb_dom_node_text_content(node, &l);
    if (!t || !mem_has_loc(t, l)) return LXB_STATUS_OK;
    if (node->type == LXB_DOM_NODE_TYPE_COMMENT) { c->is_comment = 1; c->found = 1; return LXB_STATUS_STOP; }
    lxb_dom_node_t *par = node->parent;   /* the element whose text content the hole sits in (rawtext or normal) */
    if (par && par->type == LXB_DOM_NODE_TYPE_ELEMENT) {
        size_t nl = 0; const lxb_char_t *nm = lxb_dom_element_qualified_name(lxb_dom_interface_element(par), &nl);
        if (nm && nl > 0 && nl < sizeof(c->tag)) { memcpy(c->tag, nm, nl); c->tag[nl] = 0; c->found = 1; return LXB_STATUS_STOP; }
    }
    return LXB_STATUS_OK;
}
const char *elem_fire_event(const char *tag) {
    if (!tag) return NULL;
    if (!strcmp(tag, "img") || !strcmp(tag, "input") || !strcmp(tag, "iframe") || !strcmp(tag, "embed") ||
        !strcmp(tag, "object") || !strcmp(tag, "script") || !strcmp(tag, "link") || !strcmp(tag, "source")) return "onerror";
    if (!strcmp(tag, "body") || !strcmp(tag, "svg") || !strcmp(tag, "video") || !strcmp(tag, "audio")) return "onload";
    return NULL;
}
void solve_ctx_detect(const char *shape, struct ctx_probe *out) {
    if (!shape || !out) return;
    size_t sl = strlen(shape);
    char *wl = (char *)malloc(sl + 16); CHECK(wl, "ctx-detect-oom: malloc failed -> a half-filled ctx_probe yields the wrong breakout");   /* the shape with each {..} hole replaced by the locator */
    size_t o = 0;
    for (size_t i = 0; i < sl; ) {
        if (shape[i] == '{') { const char *e = strchr(shape + i, '}'); if (e) { size_t Ln = strlen(CTX_LOC); if (o + Ln < sl + 15) { memcpy(wl + o, CTX_LOC, Ln); o += Ln; } i = (size_t)(e - shape) + 1; continue; } }
        if (o < sl + 15) wl[o++] = shape[i]; i++;
    }
    wl[o] = 0;
    lxb_html_document_t *doc = lxb_html_document_create();
    if (doc) {
        if (lxb_html_document_parse(doc, (const lxb_char_t *)wl, o) == LXB_STATUS_OK)
            lxb_dom_node_simple_walk(lxb_dom_interface_node(doc), ctx_probe_cb, out);
        lxb_html_document_destroy(doc);
    }
    free(wl);
}
