/* DOM Element JSClass + el_wrap — see dom_element.h.
 *
 * The wrapper is deliberately LOGIC-FREE: it only binds a real Lexbor node to a JS Element object whose
 * methods (still in main.c, migrating here) are DOM host-edges the ONE BFS scheduler drives — el_wrap adds
 * no control flow of its own, so no logic ever lives outside the scheduler. */
#include <string.h>
#include <lexbor/html/html.h>   /* lxb_html_template_element_t for <template>.content */
#include "core/dom/dom_element.h"
#include "core/dom/dom_select.h"   /* dom_node_matches — matches()/closest() run the real CSS selector */
#include "solver/solve.h"        /* solve_add — setAttribute(on*/href/src) + innerHTML/insertAdjacentHTML are @S sinks */
#include "solver/dom_cow.h"      /* dom_attr_capture — an attribute write joins the per-flow COW delta */
#include "solver/attr_shadow.h"  /* attr_shadow_find/set/opaque — a value set via attr keeps its taint+example */
#include "solver/concolic.h"       /* g_concolic — el.innerHTML read is opaque external input */
#include "core/css/cssom.h"        /* js_el_inline_style — el.style is a per-flow inline CSSStyleDeclaration */
#include "core/html/html_script_element.h"   /* script_maybe_load — an appended <script src> is a discovered chunk */
#include "core/html/forms/forms.h"        /* js_form_submit — el.submit()/requestSubmit() fires the form's @H action request */
#include "solver/concolic.h"       /* js_noop — UI no-op element methods (click/focus/removeAttribute...) */
#include "platform/url.h"          /* (via html_script_element) */
#include "core/dom/classlist.h"    /* js_el_classlist_get — el.classList (a real class-attr token check) */
#include "check.h"                 /* DCHECK — the live document is an engine invariant (created at boot) */

/* Borrowed from main.c: an @S replay flow pins the concrete candidate here (a reflected-property write in a
   replay writes the candidate, not the shadow taint). */
extern char *g_candidate;
extern lxb_html_document_t *g_dom;   /* the live parsed document (main.c) — attachShadow/createElement create in it */
extern JSValue js_add_listener(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv);   /* register a handler -> orphan-driven (scheduler edge, main.c) */

JSClassID g_el_class_id;   /* the "Element" JSClass id (lexbor owns the nodes; no JS finalizer) */

JSValue el_wrap(JSContext *ctx, lxb_dom_element_t *el) {
    if (!el) return JS_NULL;
    JSValue o = JS_NewObjectClass(ctx, g_el_class_id);
    if (JS_IsException(o)) return o;
    JS_SetOpaque(o, el);
    return o;
}

/* DOM TRAVERSAL: parentNode/children/firstElementChild/nextElementSibling were undefined -> `el.children
   .length` / `el.parentNode.x` THREW, killing DOM-walking bundles. Return REAL el_wrap'd element nodes from
   Lexbor so a tree walk that reaches a fetch/sink is explored. children is a REAL array (.length/.forEach/[i]
   all work). Only ELEMENT nodes (text nodes aren't wrapped — a walker keying on .children matches the browser). */
static int node_is_element(lxb_dom_node_t *n) { return n && n->type == LXB_DOM_NODE_TYPE_ELEMENT; }
JSValue js_el_parent(JSContext *ctx, JSValueConst this_val) {
    lxb_dom_element_t *el = JS_GetOpaque(this_val, g_el_class_id); if (!el) return JS_NULL;
    lxb_dom_node_t *p = lxb_dom_interface_node(el)->parent;
    return node_is_element(p) ? el_wrap(ctx, lxb_dom_interface_element(p)) : JS_NULL;
}
JSValue js_el_children(JSContext *ctx, JSValueConst this_val) {
    lxb_dom_element_t *el = JS_GetOpaque(this_val, g_el_class_id);
    JSValue arr = JS_NewArray(ctx); if (!el) return arr;
    uint32_t idx = 0;
    for (lxb_dom_node_t *n = lxb_dom_interface_node(el)->first_child; n; n = n->next)
        if (node_is_element(n)) JS_SetPropertyUint32(ctx, arr, idx++, el_wrap(ctx, lxb_dom_interface_element(n)));
    return arr;
}
JSValue js_el_first_el_child(JSContext *ctx, JSValueConst this_val) {
    lxb_dom_element_t *el = JS_GetOpaque(this_val, g_el_class_id); if (!el) return JS_NULL;
    for (lxb_dom_node_t *n = lxb_dom_interface_node(el)->first_child; n; n = n->next)
        if (node_is_element(n)) return el_wrap(ctx, lxb_dom_interface_element(n));
    return JS_NULL;
}
JSValue js_el_next_el_sib(JSContext *ctx, JSValueConst this_val) {
    lxb_dom_element_t *el = JS_GetOpaque(this_val, g_el_class_id); if (!el) return JS_NULL;
    for (lxb_dom_node_t *n = lxb_dom_interface_node(el)->next; n; n = n->next)
        if (node_is_element(n)) return el_wrap(ctx, lxb_dom_interface_element(n));
    return JS_NULL;
}
JSValue js_el_matches(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    lxb_dom_element_t *el = JS_GetOpaque(this_val, g_el_class_id);
    if (!el || argc < 1) return JS_FALSE;
    const char *s = JS_ToCString(ctx, argv[0]); if (!s) return JS_FALSE;
    int m = dom_node_matches(el, s, strlen(s)); JS_FreeCString(ctx, s);
    return m ? JS_TRUE : JS_FALSE;
}
JSValue js_el_closest(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    lxb_dom_element_t *el = JS_GetOpaque(this_val, g_el_class_id);
    if (!el || argc < 1) return JS_NULL;
    const char *s = JS_ToCString(ctx, argv[0]); size_t sl = s ? strlen(s) : 0;
    JSValue r = JS_NULL;
    for (lxb_dom_node_t *n = lxb_dom_interface_node(el); n && n->type == LXB_DOM_NODE_TYPE_ELEMENT; n = lxb_dom_node_parent(n))
        if (s && dom_node_matches(lxb_dom_interface_element(n), s, sl)) { r = el_wrap(ctx, lxb_dom_interface_element(n)); break; }
    if (s) JS_FreeCString(ctx, s);
    return r;
}
JSValue js_el_has_attr(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    lxb_dom_element_t *el = JS_GetOpaque(this_val, g_el_class_id);
    if (!el || argc < 1) return JS_FALSE;
    const char *n = JS_ToCString(ctx, argv[0]); if (!n) return JS_FALSE;
    bool h = lxb_dom_element_has_attribute(el, (const lxb_char_t *)n, strlen(n)); JS_FreeCString(ctx, n);
    return h ? JS_TRUE : JS_FALSE;
}
JSValue js_el_contains(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    lxb_dom_element_t *self = JS_GetOpaque(this_val, g_el_class_id);
    lxb_dom_element_t *other = (argc >= 1) ? JS_GetOpaque(argv[0], g_el_class_id) : NULL;
    if (!self || !other) return JS_FALSE;
    for (lxb_dom_node_t *n = lxb_dom_interface_node(other); n; n = lxb_dom_node_parent(n))
        if (n == lxb_dom_interface_node(self)) return JS_TRUE;
    return JS_FALSE;
}
JSValue js_el_style_get(JSContext *ctx, JSValueConst this_val) { return js_el_inline_style(ctx, this_val); }   /* per-flow inline style (browser/cssom.c), backed by the COW-captured `style` attribute */
/* template.content: the inert DocumentFragment holding the <template>'s parsed children. Apps instantiate a
   template via template.content.cloneNode(true) / .querySelector(...) (the core <template> / lit-html / web-
   component idiom); undefined here made that a fatal TypeError. Wrap the Lexbor content fragment (a node) so
   querySelector/childNodes/firstElementChild traverse the inert content and handlers wired to it are driven. */
JSValue js_el_content_get(JSContext *ctx, JSValueConst this_val) {
    lxb_dom_element_t *el = JS_GetOpaque(this_val, g_el_class_id);
    if (!el) return JS_UNDEFINED;
    size_t nl = 0; const lxb_char_t *nm = lxb_dom_element_qualified_name(el, &nl);
    if (!nm || nl != 8 || memcmp(nm, "template", 8) != 0) return JS_UNDEFINED;   /* .content only exists on <template> */
    lxb_dom_document_fragment_t *frag = ((lxb_html_template_element_t *)el)->content;
    if (!frag) return JS_UNDEFINED;
    return el_wrap(ctx, (lxb_dom_element_t *)frag);   /* fragment IS a node; querySelector/childNodes walk it */
}
static const char *refl_name(int magic) {   /* property magic -> the ATTRIBUTE it reflects (className -> class) */
    switch (magic) {
        case 0: return "src"; case 1: return "href"; case 2: return "action"; case 3: return "id";
        case 4: return "value"; case 5: return "name"; case 6: return "type"; case 7: return "class";
        case 8: return "alt"; case 9: return "title"; case 10: return "placeholder"; case 11: return "srcdoc";
        case 12: return "nonce";   /* HTMLElement.nonce IDL attr — returns the real value (unlike getAttribute, which nonce-hiding empties); a gadget reusing script.nonce is a real XSS path the solver follows */
        default: return "";
    }
}
/* BOOLEAN reflected props (checked/disabled/hidden/...): the PROPERTY is true iff the attribute is present.
   undefined was falsy (no throw) but wrong — `el.checked===true` / faithful form state needs the real bool. */
static const char *bool_name(int magic) {
    switch (magic) { case 0: return "checked"; case 1: return "disabled"; case 2: return "hidden";
        case 3: return "selected"; case 4: return "required"; case 5: return "readonly"; case 6: return "multiple"; default: return ""; }
}
JSValue js_el_bool_get(JSContext *ctx, JSValueConst this_val, int magic) {
    lxb_dom_element_t *el = JS_GetOpaque(this_val, g_el_class_id); if (!el) return JS_FALSE;
    const char *n = bool_name(magic);
    return lxb_dom_element_has_attribute(el, (const lxb_char_t *)n, strlen(n)) ? JS_TRUE : JS_FALSE;
}
/* tagName / nodeName: the element's tag, UPPERCASE for HTML (spec) — Lexbor lowercases. Real bundles branch
   on el.tagName constantly (`if(el.tagName==='A')`); undefined broke that. */
JSValue js_el_tagname(JSContext *ctx, JSValueConst this_val) {
    lxb_dom_element_t *el = JS_GetOpaque(this_val, g_el_class_id); if (!el) return JS_UNDEFINED;
    size_t nl = 0; const lxb_char_t *nm = lxb_dom_element_qualified_name(el, &nl);
    if (!nm) return JS_NewString(ctx, "");
    char buf[64]; size_t n = nl < 63 ? nl : 63;
    for (size_t i = 0; i < n; i++) { char c = (char)nm[i]; buf[i] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }
    buf[n] = 0;
    return JS_NewString(ctx, buf);
}
/* A FREE-TEXT editable form control whose `.value` the USER (attacker) types — <textarea>, or an <input> of a
   free-text type (text/search/tel/url/email/password or no type). NOT readonly/disabled (page-fixed), NOT hidden
   (page-controlled, e.g. a CSRF token — modelling it attacker would wrongly shape a real @H value), NOT
   select/checkbox/radio (value constrained to page-defined options). So reading such a `.value` is attacker input
   -> a DOM-XSS source (el.innerHTML = input.value). */
static int el_is_freetext_control(lxb_dom_element_t *el) {
    size_t tl = 0; const lxb_char_t *tag = lxb_dom_element_qualified_name(el, &tl);
    if (!tag) return 0;
    if (lxb_dom_element_has_attribute(el, (const lxb_char_t *)"readonly", 8) ||
        lxb_dom_element_has_attribute(el, (const lxb_char_t *)"disabled", 8)) return 0;
    if (tl == 8 && memcmp(tag, "textarea", 8) == 0) return 1;                 /* always free text */
    if (!(tl == 5 && memcmp(tag, "input", 5) == 0)) return 0;
    size_t yl = 0; const lxb_char_t *ty = lxb_dom_element_get_attribute(el, (const lxb_char_t *)"type", 4, &yl);
    if (!ty || !yl) return 1;                                                 /* no type -> defaults to text */
    static const char *ft[] = { "text", "search", "tel", "url", "email", "password" };
    for (int i = 0; i < (int)(sizeof ft / sizeof ft[0]); i++) {
        size_t fl = strlen(ft[i]);
        if (yl == fl) { size_t k = 0; for (; k < fl; k++) { char c = (char)ty[k]; if (c >= 'A' && c <= 'Z') c += 32; if (c != ft[i][k]) break; } if (k == fl) return 1; }
    }
    return 0;   /* hidden/button/submit/checkbox/radio/file/date/number/... : not free attacker text */
}
JSValue js_el_refl_get(JSContext *ctx, JSValueConst this_val, int magic) {
    lxb_dom_element_t *el = JS_GetOpaque(this_val, g_el_class_id); if (!el) return JS_UNDEFINED;
    if (magic == 4 && el_is_freetext_control(el)) {   /* value(4) of an editable free-text control = ATTACKER INPUT the user types */
        if (g_candidate) return JS_NewString(ctx, g_candidate);   /* @S replay: the user types the candidate RAW (a form value is not URL-encoded) — so the breakout reaches the sink + verifies */
        int si = attr_shadow_find(el, "value");
        if (si >= 0 && JS_IsConcolic(attr_shadow_opaque(si))) return JS_DupValue(ctx, attr_shadow_opaque(si));   /* the page SET a tainted value -> keep its real source (e.g. a reply field), not {formvalue} */
        size_t dl = 0; const lxb_char_t *dv = lxb_dom_element_get_attribute(el, (const lxb_char_t *)"value", 5, &dl);   /* the HTML default value = the example, IF present */
        JSValue o = JS_NewConcolicSourced(ctx, "{formvalue}", "{formvalue}");
        if (JS_IsConcolic(o) && dv && dl) JS_SetConcolicExample(ctx, o, JS_NewStringLen(ctx, (const char *)dv, dl));   /* a real default -> example; NO default -> stay a {formvalue} SHAPE (attacker input, no learnable value), not an empty string */
        return o;
    }
    const char *n = refl_name(magic); size_t vl = 0;
    const lxb_char_t *v = lxb_dom_element_get_attribute(el, (const lxb_char_t *)n, strlen(n), &vl);
    return v ? JS_NewStringLen(ctx, (const char *)v, vl) : JS_NewString(ctx, "");
}
JSValue js_el_refl_set(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic) {
    lxb_dom_element_t *el = JS_GetOpaque(this_val, g_el_class_id); if (!el) return JS_UNDEFINED;
    const char *n = refl_name(magic);
    if (magic == 1 || magic == 2) solve_add(ctx, "href", "url", val);   /* @S: el.href/.action = external -> javascript:/redirect */
    else if (magic == 11) solve_add(ctx, "srcdoc", "htmls", val);        /* @S: iframe.srcdoc renders attacker HTML in the frame */
    else if (magic == 0) {   /* @S: a <script>'s src loads+EXECUTES JS from its origin -> attacker-controlled origin is script injection (img/link src is not) */
        size_t tl = 0; const lxb_char_t *tag = lxb_dom_element_qualified_name(el, &tl);
        if (tl == 6 && tag && memcmp(tag, "script", 6) == 0) solve_add(ctx, "script.src", "scripturl", val);
    }
    int is_opq = JS_IsConcolic(val);
    /* A CONCOLIC value set via PROPERTY (`s.src = replyField` / `el.href = computedUrl`) must keep its real
       value in the attribute shadow — EXACTLY like setAttribute — else the concrete example is lost to the
       holey shape written into Lexbor: getAttribute would not round-trip the taint, and script_maybe_load
       could not chunk-load a reply-driven <script src> by its example. The Lexbor attr stores the display
       shape; the shadow carries the concolic (taint + example). */
    dom_attr_capture(el, n);   /* capture pre-write baseline (attr + taint shadow) BEFORE mutating either — see js_el_setAttribute */
    if (!g_candidate) attr_shadow_set(ctx, el, n, is_opq ? val : JS_UNDEFINED);
    JSValue exv = is_opq ? JS_ConcolicExample(ctx, val) : JS_UNDEFINED;   /* write the concolic EXAMPLE to Lexbor so it round-trips through getAttribute (see js_el_setAttribute) */
    int ex_str = is_opq && !JS_IsUndefined(exv);
    const char *v = ex_str ? JS_ToCString(ctx, exv) : (is_opq ? JS_ConcolicShapeC(val) : JS_ToCString(ctx, val));
    if (v) lxb_dom_element_set_attribute(el, (const lxb_char_t *)n, strlen(n), (const lxb_char_t *)v, strlen(v));
    if (v && (ex_str || !is_opq)) JS_FreeCString(ctx, v);   /* ToString'd frees; JS_ConcolicShapeC internal pointer does not */
    JS_FreeValue(ctx, exv);
    return JS_UNDEFINED;
}
JSValue js_el_getAttribute(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    lxb_dom_element_t *el = JS_GetOpaque(this_val, g_el_class_id);
    if (!el || argc < 1) return JS_NULL;
    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_NULL;
    /* Nonce-hiding (CSP3): getAttribute('nonce') is emptied so it can't leak the nonce (via reflection or a CSS
       attribute-selector) — the value survives ONLY on the .nonce IDL property. Faithful to real Chrome. */
    if ((name[0]=='n'||name[0]=='N') && strlen(name)==5 && (name[1]=='o'||name[1]=='O')) {
        char lc[6]; for (int k=0;k<5;k++){ char c=name[k]; lc[k]=(c>='A'&&c<='Z')?c+32:c; } lc[5]=0;
        if (strcmp(lc,"nonce")==0) { JS_FreeCString(ctx, name); return JS_NewString(ctx, ""); }
    }
    if (!g_candidate) {   /* baseline/opaque flow: preserve taint stashed in this attr */
        int i = attr_shadow_find(el, name);
        if (i >= 0) { JS_FreeCString(ctx, name); return JS_DupValue(ctx, attr_shadow_opaque(i)); }
    }
    size_t vlen = 0;
    const lxb_char_t *v = lxb_dom_element_get_attribute(el, (const lxb_char_t *)name, strlen(name), &vlen);
    JS_FreeCString(ctx, name);
    return v ? JS_NewStringLen(ctx, (const char *)v, vlen) : JS_NULL;   /* REAL attribute value (concrete, incl candidate) */
}
JSValue js_el_querySelector(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (argc < 1) return JS_NULL;
    const char *s = JS_ToCString(ctx, argv[0]); if (!s) return JS_NULL;
    lxb_dom_element_t *self_el = JS_GetOpaque(this_val, g_el_class_id);   /* SUBTREE-scope to the receiver element */
    lxb_dom_element_t *el = dom_select_first(self_el ? lxb_dom_interface_node(self_el) : NULL, s, strlen(s));
    JS_FreeCString(ctx, s);
    return el_wrap(ctx, el);
}
JSValue js_el_setAttribute(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    lxb_dom_element_t *el = JS_GetOpaque(this_val, g_el_class_id);
    if (!el || argc < 2) return JS_UNDEFINED;
    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_UNDEFINED;
    int is_opq = JS_IsConcolic(argv[1]);
    /* Capture the TRUE pre-write baseline (attr value AND taint shadow) into the per-flow COW delta BEFORE
       mutating either — else dom_attr_capture snapshots this flow's OWN shadow write as the baseline, so a
       context-switch's unapply can't restore the real (empty) baseline and the stashed taint is lost the
       instant a switch lands between setAttribute and a later getAttribute (the DOM-stash round-trip bug). */
    dom_attr_capture(el, name);
    /* baseline/opaque flow storing OPAQUE external input -> record the taint in the shadow (concrete value ->
       clear any stale taint). A candidate flow (concrete source) writes only the real attr, not the shadow. */
    if (!g_candidate) attr_shadow_set(ctx, el, name, is_opq ? argv[1] : JS_UNDEFINED);
    /* @S: a tainted value written into an EXECUTABLE attribute. on* handler => the value IS js code (js
       context); href/src/action => a javascript: URL (url context). Called unconditionally like the other
       sinks so solve_add both DETECTS (opaque flow) and VERIFIES the breakout (candidate flow). */
    if (name[0] == 'o' && name[1] == 'n') solve_add(ctx, "setAttribute", "js", argv[1]);
    else if (!strcmp(name, "href") || !strcmp(name, "src") || !strcmp(name, "action") || !strcmp(name, "formaction"))
        solve_add(ctx, "setAttribute", "url", argv[1]);
    /* CONCOLIC EXAMPLE to Lexbor: a candidate flow's concrete payload (or a computed/reply value) must round-trip
       through getAttribute, which — in a candidate flow — bypasses the taint shadow and reads the REAL Lexbor
       attr. Writing only the display shape degraded a DOM-stashed candidate to its shape, so it never broke out
       at the downstream sink (the cross-handler / stash-and-read @S round-trip). Prefer the example; fall back to
       the shape for a pure symbol (attacker input with no example). */
    JSValue exv = is_opq ? JS_ConcolicExample(ctx, argv[1]) : JS_UNDEFINED;
    int ex_str = is_opq && !JS_IsUndefined(exv);
    const char *val = ex_str ? JS_ToCString(ctx, exv) : (is_opq ? JS_ConcolicShapeC(argv[1]) : JS_ToCString(ctx, argv[1]));
    if (val) lxb_dom_element_set_attribute(el, (const lxb_char_t *)name, strlen(name), (const lxb_char_t *)val, strlen(val));   /* baseline captured above (before the shadow write) */
    if (val && (ex_str || !is_opq)) JS_FreeCString(ctx, val);   /* ToString'd (example/concrete) frees; JS_ConcolicShapeC internal pointer does not */
    JS_FreeValue(ctx, exv);
    JS_FreeCString(ctx, name);
    return JS_UNDEFINED;
}
/* el.insertAdjacentHTML(pos, html): DOM edge (no-op for content; the security view is forced-exec, not
   a taint check on the argument). */
JSValue js_el_insertAdjacentHTML(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (argc >= 2) solve_add(ctx, "insertAdjacentHTML", "html", argv[1]);   /* @S */
    return JS_UNDEFINED;
}
JSValue js_el_set_html(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic) {
    solve_add(ctx, magic ? "outerHTML" : "innerHTML", "html", val);         /* @S */
    return JS_UNDEFINED;
}
/* el.innerHTML / outerHTML READ: serialize the REAL Lexbor subtree (model, not opaque) — a page that does
   `JSON.parse(el.innerHTML)` on an inline SSR `<script type=json>` gets the actual config, so its concrete
   values flow. innerHTML = the children's markup; outerHTML (magic) = the element + its markup. */
typedef struct { char *buf; size_t len, cap; } HtmlBuf;
static lxb_status_t html_ser_cb(const lxb_char_t *data, size_t len, void *cbctx) {
    HtmlBuf *b = cbctx;
    if (b->len + len + 1 > b->cap) { size_t nc = (b->cap ? b->cap * 2 : 256); while (nc < b->len + len + 1) nc *= 2;
        char *n = realloc(b->buf, nc); if (!n) return LXB_STATUS_ERROR_MEMORY_ALLOCATION; b->buf = n; b->cap = nc; }
    memcpy(b->buf + b->len, data, len); b->len += len;
    return LXB_STATUS_OK;
}
JSValue js_el_get_html(JSContext *ctx, JSValueConst this_val, int magic) {
    lxb_dom_element_t *el = JS_GetOpaque(this_val, g_el_class_id);
    if (!el) return js_concolic(ctx, magic ? "{outerHTML}" : "{innerHTML}", JS_UNDEFINED);
    HtmlBuf b = { NULL, 0, 0 };
    lxb_dom_node_t *node = lxb_dom_interface_node(el);
    if (magic) lxb_html_serialize_tree_cb(node, html_ser_cb, &b);                                   /* outerHTML: element + subtree */
    else for (lxb_dom_node_t *c = node->first_child; c; c = c->next) lxb_html_serialize_tree_cb(c, html_ser_cb, &b);   /* innerHTML: children */
    JSValue r = JS_NewStringLen(ctx, b.buf ? b.buf : "", b.len);
    free(b.buf);
    return r;
}
/* el.getAttributeNames(): the element's attribute names, in order — a real DOM API frameworks use to reflect
   attrs. Missing, it threw and killed the page. */
JSValue js_el_getattrnames(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    JSValue arr = JS_NewArray(ctx);
    lxb_dom_element_t *el = JS_GetOpaque(this_val, g_el_class_id);
    if (!el) return arr;
    uint32_t i = 0;
    for (lxb_dom_attr_t *a = lxb_dom_element_first_attribute(el); a; a = lxb_dom_element_next_attribute(a)) {
        size_t nl = 0; const lxb_char_t *nm = lxb_dom_attr_qualified_name(a, &nl);
        if (nm) JS_SetPropertyUint32(ctx, arr, i++, JS_NewStringLen(ctx, (const char *)nm, nl));
    }
    return arr;
}
JSValue js_el_querySelectorAll(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (argc < 1) return JS_NewArray(ctx);
    const char *s = JS_ToCString(ctx, argv[0]); if (!s) return JS_NewArray(ctx);
    lxb_dom_element_t *self_el = JS_GetOpaque(this_val, g_el_class_id);
    JSValue r = dom_select_all(ctx, self_el ? lxb_dom_interface_node(self_el) : NULL, s, strlen(s));
    JS_FreeCString(ctx, s); return r;
}
/* getBoundingClientRect: with no layout engine the box is UNKNOWABLE (a real headless computes it; we can't),
   so each field is CONCOLIC carrying 0 as the example — a visibility/geometry gate `if(rect.width>0)` FORKS
   both the visible AND hidden worlds (the branch a bare-concrete 0 would delete), while `'/t/'+rect.top` still
   yields `/t/0`. Bare-concrete 0 collapsed every geometry branch to one arm. */
JSValue js_el_rect(JSContext *ctx, JSValueConst t, int c, JSValueConst *v) {
    JSValue o = JS_NewObject(ctx);
    const char *k[]  = { "top", "left", "right", "bottom", "width", "height", "x", "y" };
    /* DISTINCT source identity PER FIELD (not a shared "{rect}") — width and height are INDEPENDENT quantities,
       so a shared src would wrongly PIN height when a gate pins width (cons_eval_pinned keys on src). Static
       literals keep the src DETERMINISTIC (a per-call counter would be non-deterministic across replays -> the
       recorded constraint would not map back, breaking replay). */
    const char *sh[] = { "{rect.top}", "{rect.left}", "{rect.right}", "{rect.bottom}", "{rect.width}", "{rect.height}", "{rect.x}", "{rect.y}" };
    for (int i = 0; i < 8; i++) JS_SetPropertyStr(ctx, o, k[i], js_concolic(ctx, sh[i], JS_NewInt32(ctx, 0)));
    return o;
}
JSValue js_el_textContent(JSContext *ctx, JSValueConst this_val) {   /* .textContent / .innerText getter */
    lxb_dom_element_t *el = JS_GetOpaque(this_val, g_el_class_id);
    if (!el) return JS_NULL;
    size_t len = 0;
    lxb_char_t *txt = lxb_dom_node_text_content(lxb_dom_interface_node(el), &len);
    if (!txt) return JS_NewString(ctx, "");
    JSValue r = JS_NewStringLen(ctx, (const char *)txt, len);
    lxb_dom_document_destroy_text(lxb_dom_interface_node(el)->owner_document, txt);
    /* SSR DATA: a <script type=...json...> blob is server-injected app data (the TRUST boundary, like a
       reply) — declared DATA, not executable code. Return it CONCOLIC so JSON.parse of it FORKS gates on
       loaded values while fields keep real examples. Matched by the server-DECLARED MIME type (structural). */
    size_t nl = 0; const lxb_char_t *nm = lxb_dom_element_qualified_name(el, &nl);
    if (nm && nl == 6 && !memcmp(nm, "script", 6)) {
        size_t tl = 0; const lxb_char_t *ty = lxb_dom_element_get_attribute(el, (const lxb_char_t *)"type", 4, &tl);
        int is_json = 0;
        for (size_t i = 0; ty && i + 4 <= tl; i++) if (!memcmp(ty + i, "json", 4)) { is_json = 1; break; }
        if (is_json) {
            JSValue o = JS_NewConcolicSourced(ctx, "{ssr}", "{ssr}");
            if (JS_IsConcolic(o)) { JS_SetConcolicExample(ctx, o, r); return o; }   /* consumes r */
            JS_FreeValue(ctx, o);
        }
    }
    return r;
}
JSValue js_el_dataset_get(JSContext *ctx, JSValueConst this_val) {
    lxb_dom_element_t *el = JS_GetOpaque(this_val, g_el_class_id);
    JSValue o = JS_NewObject(ctx);
    if (!el) return o;
    for (lxb_dom_attr_t *a = lxb_dom_element_first_attribute(el); a; a = lxb_dom_element_next_attribute(a)) {
        size_t nlen; const lxb_char_t *nm = lxb_dom_attr_qualified_name(a, &nlen);
        if (!nm || nlen <= 5 || memcmp(nm, "data-", 5) != 0) continue;
        char key[128]; int ki = 0;                                   /* data-foo-bar -> fooBar */
        for (size_t i = 5; i < nlen && ki < 126; i++) {
            if (nm[i] == '-' && i + 1 < nlen) { i++; char c = (char)nm[i]; key[ki++] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }
            else key[ki++] = (char)nm[i];
        }
        key[ki] = 0;
        size_t vlen; const lxb_char_t *v = lxb_dom_attr_value(a, &vlen);
        JS_SetPropertyStr(ctx, o, key, v ? JS_NewStringLen(ctx, (const char *)v, vlen) : JS_NewString(ctx, ""));
    }
    return o;
}

/* ── Node tree mutation + Shadow DOM + on<event> content attributes + the Element binding install ───────────
 * Moved out of the scheduler (main.c): these are Element/Node components. They FEED the ONE scheduler via
 * extern edges — solve_add records the appendChild @S sink, js_add_listener parks a handler-flow, script_maybe_
 * load queues a discovered chunk — the scheduler decides; the component holds no control flow of its own.
 * (In Blink these span ContainerNode/Node/Element/GlobalEventHandlers; el_wrap merges them into one class.) */

/* on<event> content-attribute handlers: `el.onclick = fn` attaches to the (persistent) element in a real
   browser, firing regardless of whether the app keeps the JS wrapper. Register via the SAME path as
   addEventListener (-> g_handlers -> orphan-driven) so an onX set on a transient wrapper is not lost. */
static const char *ON_EVENTS[] = {
    "click","dblclick","mousedown","mouseup","mouseover","mouseout","mouseenter","mouseleave","mousemove",
    "keydown","keyup","keypress","submit","change","input","focus","blur","load","error","message",
    "scroll","resize","touchstart","touchend","touchmove","pointerdown","pointerup","pointermove",
    "contextmenu","readystatechange","animationend","transitionend","dragstart","dragend","drop",
    "paste","copy","cut","wheel","play","pause","ended","canplay","loadeddata"
};
#define N_ON_EVENTS ((int)(sizeof ON_EVENTS / sizeof ON_EVENTS[0]))
static JSValue js_el_on_set(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic) {
    if (JS_IsFunction(ctx, val) && magic >= 0 && magic < N_ON_EVENTS) {
        JSValue tv = JS_NewString(ctx, ON_EVENTS[magic]);
        JSValueConst a[2] = { tv, val };
        js_add_listener(ctx, this_val, 2, a);   /* same registration path -> driven; 'message' tracking too */
        JS_FreeValue(ctx, tv);
    }
    return JS_UNDEFINED;
}

/* ContainerNode::appendChild (+ insertBefore/replaceChild/before/after wire here): insert into the live DOM,
   capture the insertion into the per-flow DOM COW delta, and discover an injected <script src>. Inserting a
   {parsedhtml}-TAINTED node (DOMParser/Range of attacker input) into the live DOM is the @S sink. */
static JSValue js_el_appendChild(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    lxb_dom_element_t *parent = JS_GetOpaque(this_val, g_el_class_id);
    if (argc > 0 && JS_IsConcolic(argv[0])) {
        const char *sh = JS_ConcolicShapeC(argv[0]);
        if (sh && strcmp(sh, "{parsedhtml}") == 0) solve_add(ctx, "appendChild", "html", argv[0]);
    }
    lxb_dom_element_t *child = (argc > 0) ? JS_GetOpaque(argv[0], g_el_class_id) : NULL;
    if (parent && child) {
        lxb_dom_node_insert_child(lxb_dom_interface_node(parent), lxb_dom_interface_node(child));
        dom_insert_capture(lxb_dom_interface_node(child));
        script_maybe_load(ctx, child);   /* injected <script src> (computed URL) -> discovered as a chunk */
    }
    return (argc > 0) ? JS_DupValue(ctx, argv[0]) : JS_UNDEFINED;
}

/* Generic identity (returns `this`). NOT for DOM cloneNode (see js_el_clone) — only for Request.clone in
   urlobj.c, a plain non-el object whose self-return is a documented shallow-clone stub tracked separately. */
JSValue js_el_self(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) { (void)argc; (void)argv; return JS_DupValue(ctx, this_val); }
/* Node.cloneNode(deep): a REAL Lexbor clone (independent subtree), NEVER the same node. The old js_el_self
   returned JS_DupValue(this_val) — the SAME node — so template.content.cloneNode(true) ALIASED the template
   and a clone-then-mutate (web components / lit-html, the core <template> idiom) corrupted the live template.
   The clone is DETACHED (import_node does not insert), so no COW capture here; its later appendChild IS
   captured, so it time-travels correctly once in the tree. */
JSValue js_el_clone(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    lxb_dom_element_t *el = JS_GetOpaque(this_val, g_el_class_id);
    DCHECK(el, "cloneNode: receiver has no el-backed node — cloneNode lives on the Element proto, so this_val is always el_wrap'd");
    int deep = argc >= 1 ? JS_ToBool(ctx, argv[0]) : 0;   /* cloneNode() defaults deep=false (spec) */
    lxb_dom_node_t *clone = lxb_dom_node_clone(lxb_dom_interface_node(el), deep > 0);
    CHECK(clone, "cloneNode: lxb_dom_node_clone allocation failed — OOM is a physical floor");
    return el_wrap(ctx, lxb_dom_interface_element(clone));
}

/* Element::attachShadow(init): the Shadow DOM root nearly every custom element renders into. A real detached
   container so root.appendChild/querySelector/addEventListener register handlers driven like any other. */
static JSValue js_el_attach_shadow(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)argc; (void)argv;
    DCHECK(g_dom, "attachShadow: g_dom NULL — the live document is created at boot before any script, impossible");
    lxb_dom_element_t *root = lxb_dom_document_create_element(lxb_dom_interface_document(g_dom), (const lxb_char_t *)"shadow-root", 11, NULL);
    if (!root) return JS_UNDEFINED;
    JSValue rv = el_wrap(ctx, root);
    JS_SetPropertyStr(ctx, (JSValue)this_val, "shadowRoot", JS_DupValue(ctx, rv));   /* host.shadowRoot (open) */
    return rv;
}

/* The Element interface binding install (Blink generates this from Element.idl): wire every method + reflected
   property + on<event> setter onto the element prototype. Pure-read methods live above in this file; the
   scheduler-coupled edges (appendChild/@S, addEventListener/orphan, submit/@H) are wired here by call. */
/* A DOM operation with NO observable SCRIPT result in a headless engine (no user, no viewport, no layout):
   click/focus/blur/scrollIntoView per spec return undefined and their effect (activation, focus ring, scroll)
   is non-scriptable here — a faithful headless implementation, DEDICATED + documented, never a lazy noop stub. */
static JSValue js_el_ui_noeffect(JSContext *ctx, JSValueConst t, int c, JSValueConst *v) { (void)ctx; (void)t; (void)c; (void)v; return JS_UNDEFINED; }
/* removeEventListener/dispatchEvent: we keep every handler REACHABLE for orphan-driving and drive by exploration
   rather than honour removal or fire a synthetic event — a conscious analysis decision, documented. */
static JSValue js_el_evt_noeffect(JSContext *ctx, JSValueConst t, int c, JSValueConst *v) { (void)ctx; (void)t; (void)c; (void)v; return JS_UNDEFINED; }
/* removeAttribute(name): REAL — remove the attribute from the live Lexbor element (per spec, not a noop). */
static JSValue js_el_removeAttribute(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    lxb_dom_element_t *el = JS_GetOpaque(this_val, g_el_class_id);
    if (el && argc >= 1) { const char *n = JS_ToCString(ctx, argv[0]);
        if (n) { lxb_dom_element_remove_attribute(el, (const lxb_char_t *)n, strlen(n)); JS_FreeCString(ctx, n); } }
    return JS_UNDEFINED;
}

/* NOTE: this hand install remains until Element is converted to the generated codegen (element.gen.{c,h}) like
   AbortSignal/the observers — the flagship, pending its magic reflected attrs + HTMLElement split. Every op here
   is a REAL impl or a dedicated documented no-effect, never the generic js_noop. */
void el_install_methods(JSContext *ctx, JSValue proto) {
    JS_SetPropertyStr(ctx, proto, "getAttribute", JS_NewCFunction(ctx, js_el_getAttribute, "getAttribute", 1));
    JS_SetPropertyStr(ctx, proto, "setAttribute", JS_NewCFunction(ctx, js_el_setAttribute, "setAttribute", 2));
    JS_SetPropertyStr(ctx, proto, "removeAttribute", JS_NewCFunction(ctx, js_el_removeAttribute, "removeAttribute", 1));
    JS_SetPropertyStr(ctx, proto, "appendChild", JS_NewCFunction(ctx, js_el_appendChild, "appendChild", 1));
    JS_SetPropertyStr(ctx, proto, "insertBefore", JS_NewCFunction(ctx, js_el_appendChild, "insertBefore", 2));
    JS_SetPropertyStr(ctx, proto, "replaceChild", JS_NewCFunction(ctx, js_el_appendChild, "replaceChild", 2));
    JS_SetPropertyStr(ctx, proto, "before", JS_NewCFunction(ctx, js_el_appendChild, "before", 1));
    JS_SetPropertyStr(ctx, proto, "after", JS_NewCFunction(ctx, js_el_appendChild, "after", 1));
    JS_SetPropertyStr(ctx, proto, "append", JS_NewCFunction(ctx, js_el_appendChild, "append", 1));
    JS_SetPropertyStr(ctx, proto, "prepend", JS_NewCFunction(ctx, js_el_appendChild, "prepend", 1));
    JS_SetPropertyStr(ctx, proto, "insertAdjacentHTML", JS_NewCFunction(ctx, js_el_insertAdjacentHTML, "insertAdjacentHTML", 2));
    JS_SetPropertyStr(ctx, proto, "querySelector", JS_NewCFunction(ctx, js_el_querySelector, "querySelector", 1));
    JS_SetPropertyStr(ctx, proto, "querySelectorAll", JS_NewCFunction(ctx, js_el_querySelectorAll, "querySelectorAll", 1));
    JS_SetPropertyStr(ctx, proto, "getElementsByTagName", JS_NewCFunction(ctx, js_el_querySelectorAll, "getElementsByTagName", 1));
    JS_SetPropertyStr(ctx, proto, "matches", JS_NewCFunction(ctx, js_el_matches, "matches", 1));
    JS_SetPropertyStr(ctx, proto, "webkitMatchesSelector", JS_NewCFunction(ctx, js_el_matches, "webkitMatchesSelector", 1));
    JS_SetPropertyStr(ctx, proto, "closest", JS_NewCFunction(ctx, js_el_closest, "closest", 1));
    JS_SetPropertyStr(ctx, proto, "cloneNode", JS_NewCFunction(ctx, js_el_clone, "cloneNode", 1));
    JS_SetPropertyStr(ctx, proto, "contains", JS_NewCFunction(ctx, js_el_contains, "contains", 1));
    JS_SetPropertyStr(ctx, proto, "hasAttribute", JS_NewCFunction(ctx, js_el_has_attr, "hasAttribute", 1));
    JS_SetPropertyStr(ctx, proto, "toggleAttribute", JS_NewCFunction(ctx, js_el_has_attr, "toggleAttribute", 1));
    JS_SetPropertyStr(ctx, proto, "getAttributeNames", JS_NewCFunction(ctx, js_el_getattrnames, "getAttributeNames", 0));
    JS_SetPropertyStr(ctx, proto, "attachShadow", JS_NewCFunction(ctx, js_el_attach_shadow, "attachShadow", 1));
    JS_SetPropertyStr(ctx, proto, "getBoundingClientRect", JS_NewCFunction(ctx, js_el_rect, "getBoundingClientRect", 0));
    /* addEventListener/removeEventListener/dispatchEvent are INHERITED from EventTarget.prototype — el_proto is
       chained to the spine root after event_target_init (an Element IS an EventTarget), so they are NOT installed
       here (they'd shadow the shared method and break `element.addEventListener === EventTarget.prototype.…`). */
    JS_SetPropertyStr(ctx, proto, "submit", JS_NewCFunction(ctx, js_form_submit, "submit", 0));
    JS_SetPropertyStr(ctx, proto, "requestSubmit", JS_NewCFunction(ctx, js_form_submit, "requestSubmit", 1));
    JS_SetPropertyStr(ctx, proto, "click", JS_NewCFunction(ctx, js_el_ui_noeffect, "click", 0));
    JS_SetPropertyStr(ctx, proto, "focus", JS_NewCFunction(ctx, js_el_ui_noeffect, "focus", 0));
    JS_SetPropertyStr(ctx, proto, "blur", JS_NewCFunction(ctx, js_el_ui_noeffect, "blur", 0));
    JS_SetPropertyStr(ctx, proto, "scrollIntoView", JS_NewCFunction(ctx, js_el_ui_noeffect, "scrollIntoView", 0));
    JS_SetPropertyStr(ctx, proto, "remove", JS_NewCFunction(ctx, js_el_ui_noeffect, "remove", 0));
    for (int i = 0; i < 2; i++) {   /* textContent / innerText getters (SSR data: JSON.parse(script.textContent)) */
        JSAtom a = JS_NewAtom(ctx, i ? "innerText" : "textContent");
        JS_DefinePropertyGetSet(ctx, proto, a, JS_NewCFunction2(ctx, (JSCFunction *)js_el_textContent, "get", 0, JS_CFUNC_getter, 0), JS_UNDEFINED, JS_PROP_CONFIGURABLE);
        JS_FreeAtom(ctx, a);
    }
    { JSAtom a = JS_NewAtom(ctx, "style");
      JS_DefinePropertyGetSet(ctx, proto, a, JS_NewCFunction2(ctx, (JSCFunction *)js_el_style_get, "get style", 0, JS_CFUNC_getter, 0), JS_UNDEFINED, JS_PROP_CONFIGURABLE);
      JS_FreeAtom(ctx, a); }
    { JSAtom a = JS_NewAtom(ctx, "content");   /* template.content -> inert fragment (queryable, cloneable) */
      JS_DefinePropertyGetSet(ctx, proto, a, JS_NewCFunction2(ctx, (JSCFunction *)js_el_content_get, "get content", 0, JS_CFUNC_getter, 0), JS_UNDEFINED, JS_PROP_CONFIGURABLE);
      JS_FreeAtom(ctx, a); }
    for (int i = 0; i < N_ON_EVENTS; i++) {   /* on<event> = fn -> register in g_handlers (driven regardless of wrapper reachability) */
        char nm[40]; snprintf(nm, sizeof nm, "on%s", ON_EVENTS[i]);
        JSAtom a = JS_NewAtom(ctx, nm);
        JS_DefinePropertyGetSet(ctx, proto, a, JS_UNDEFINED,
            JS_NewCFunctionMagic(ctx, (JSCFunctionMagic *)js_el_on_set, "on-set", 1, JS_CFUNC_setter_magic, i), JS_PROP_CONFIGURABLE);
        JS_FreeAtom(ctx, a);
    }
    { JSAtom a = JS_NewAtom(ctx, "dataset");
      JS_DefinePropertyGetSet(ctx, proto, a, JS_NewCFunction2(ctx, (JSCFunction *)js_el_dataset_get, "get dataset", 0, JS_CFUNC_getter, 0), JS_UNDEFINED, JS_PROP_CONFIGURABLE);
      JS_FreeAtom(ctx, a); }
    for (int i = 0; i < 2; i++) {   /* innerHTML (magic 0) / outerHTML (magic 1) setter = XSS sink */
        JSAtom a = JS_NewAtom(ctx, i ? "outerHTML" : "innerHTML");
        JS_DefinePropertyGetSet(ctx, proto, a,
            JS_NewCFunctionMagic(ctx, (JSCFunctionMagic *)js_el_get_html, "get", 0, JS_CFUNC_getter_magic, i),
            JS_NewCFunctionMagic(ctx, (JSCFunctionMagic *)js_el_set_html, "set", 1, JS_CFUNC_setter_magic, i),
            JS_PROP_CONFIGURABLE);
        JS_FreeAtom(ctx, a);
    }
    static const char *refl[] = { "src", "href", "action", "id", "value", "name", "type", "className", "alt", "title", "placeholder", "srcdoc", "nonce" };
    for (int i = 0; i < (int)(sizeof refl / sizeof refl[0]); i++) {
        JSAtom a = JS_NewAtom(ctx, refl[i]);
        JS_DefinePropertyGetSet(ctx, proto, a,
            JS_NewCFunctionMagic(ctx, (JSCFunctionMagic *)js_el_refl_get, "get", 0, JS_CFUNC_getter_magic, i),
            JS_NewCFunctionMagic(ctx, (JSCFunctionMagic *)js_el_refl_set, "set", 1, JS_CFUNC_setter_magic, i),
            JS_PROP_CONFIGURABLE);
        JS_FreeAtom(ctx, a);
    }
    for (int i = 0; i < 2; i++) {   /* tagName / nodeName -> the uppercase tag */
        JSAtom a = JS_NewAtom(ctx, i ? "nodeName" : "tagName");
        JS_DefinePropertyGetSet(ctx, proto, a, JS_NewCFunction2(ctx, (JSCFunction *)js_el_tagname, "get", 0, JS_CFUNC_getter, 0), JS_UNDEFINED, JS_PROP_CONFIGURABLE);
        JS_FreeAtom(ctx, a);
    }
    JS_SetPropertyStr(ctx, proto, "nodeType", JS_NewInt32(ctx, 1));   /* Node.ELEMENT_NODE — real bundles branch on `n.nodeType === 1` constantly (undefined skipped that code) */
    { JSAtom a = JS_NewAtom(ctx, "classList");
      JS_DefinePropertyGetSet(ctx, proto, a, JS_NewCFunction2(ctx, (JSCFunction *)js_el_classlist_get, "get", 0, JS_CFUNC_getter, 0), JS_UNDEFINED, JS_PROP_CONFIGURABLE);
      JS_FreeAtom(ctx, a); }
    static const char *boolp[] = { "checked", "disabled", "hidden", "selected", "required", "readOnly", "multiple" };
    for (int i = 0; i < (int)(sizeof boolp / sizeof boolp[0]); i++) {   /* boolean attribute-presence props */
        JSAtom a = JS_NewAtom(ctx, boolp[i]);
        JS_DefinePropertyGetSet(ctx, proto, a, JS_NewCFunctionMagic(ctx, (JSCFunctionMagic *)js_el_bool_get, "get", 0, JS_CFUNC_getter_magic, i), JS_UNDEFINED, JS_PROP_CONFIGURABLE);
        JS_FreeAtom(ctx, a);
    }
    struct { const char *prop; JSCFunction *fn; } trav[] = {
        { "parentNode", (JSCFunction *)js_el_parent }, { "parentElement", (JSCFunction *)js_el_parent },
        { "children", (JSCFunction *)js_el_children }, { "childNodes", (JSCFunction *)js_el_children },
        { "firstChild", (JSCFunction *)js_el_first_el_child }, { "firstElementChild", (JSCFunction *)js_el_first_el_child },
        { "nextSibling", (JSCFunction *)js_el_next_el_sib }, { "nextElementSibling", (JSCFunction *)js_el_next_el_sib },
    };
    for (int i = 0; i < (int)(sizeof trav / sizeof trav[0]); i++) {
        JSAtom a = JS_NewAtom(ctx, trav[i].prop);
        JS_DefinePropertyGetSet(ctx, proto, a, JS_NewCFunction2(ctx, trav[i].fn, "get", 0, JS_CFUNC_getter, 0), JS_UNDEFINED, JS_PROP_CONFIGURABLE);
        JS_FreeAtom(ctx, a);
    }
}
