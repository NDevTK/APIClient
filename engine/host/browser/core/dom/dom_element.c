/* DOM Element JSClass + el_wrap — see dom_element.h.
 *
 * The wrapper is deliberately LOGIC-FREE: it only binds a real Lexbor node to a JS Element object whose
 * methods (still in main.c, migrating here) are DOM host-edges the ONE BFS scheduler drives — el_wrap adds
 * no control flow of its own, so no logic ever lives outside the scheduler. */
#include <string.h>
#include <lexbor/html/html.h>   /* lxb_html_template_element_t for <template>.content */
#include "core/dom/dom_element.h"
#include "core/dom/dom_select.h"   /* dom_node_matches — matches()/closest() run the real CSS selector */
#include "solve.h"        /* solve_add — setAttribute(on*/href/src) + innerHTML/insertAdjacentHTML are @S sinks */
#include "dom_cow.h"      /* dom_attr_capture — an attribute write joins the per-flow COW delta */
#include "attr_shadow.h"  /* attr_shadow_find/set/opaque — a value set via attr keeps its taint+example */
#include "opaque.h"       /* g_opaque — el.innerHTML read is opaque external input */
#include "core/css/cssom.h"        /* js_el_inline_style — el.style is a per-flow inline CSSStyleDeclaration */
#include "core/html/html_script_element.h"   /* script_maybe_load — an appended <script src> is a discovered chunk */
#include "core/html/forms/forms.h"        /* js_form_submit — el.submit()/requestSubmit() fires the form's @H action request */
#include "opaque.h"       /* js_noop — UI no-op element methods (click/focus/removeAttribute...) */
#include "platform/url.h"          /* (via html_script_element) */
#include "core/dom/classlist.h"    /* js_el_classlist_get — el.classList (a real class-attr token check) */
#include "check.h"                 /* DCHECK — the live document is an engine invariant (created at boot) */
#include "bindings/idl.h"          /* idl_bind — the Element OPERATION list is generated from canonical Element IDL */
#include "bindings/idl_generated.h"   /* Element_IDL — the spec-exact member shape */

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
JSValue js_el_refl_get(JSContext *ctx, JSValueConst this_val, int magic) {
    lxb_dom_element_t *el = JS_GetOpaque(this_val, g_el_class_id); if (!el) return JS_UNDEFINED;
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
    int is_opq = JS_IsOpaque(val);
    /* A CONCOLIC value set via PROPERTY (`s.src = replyField` / `el.href = computedUrl`) must keep its real
       value in the attribute shadow — EXACTLY like setAttribute — else the concrete example is lost to the
       holey shape written into Lexbor: getAttribute would not round-trip the taint, and script_maybe_load
       could not chunk-load a reply-driven <script src> by its example. The Lexbor attr stores the display
       shape; the shadow carries the concolic (taint + example). */
    dom_attr_capture(el, n);   /* capture pre-write baseline (attr + taint shadow) BEFORE mutating either — see js_el_setAttribute */
    if (!g_candidate) attr_shadow_set(ctx, el, n, is_opq ? val : JS_UNDEFINED);
    JSValue exv = is_opq ? JS_OpaqueExample(ctx, val) : JS_UNDEFINED;   /* write the concolic EXAMPLE to Lexbor so it round-trips through getAttribute (see js_el_setAttribute) */
    int ex_str = is_opq && !JS_IsUndefined(exv);
    const char *v = ex_str ? JS_ToCString(ctx, exv) : (is_opq ? JS_OpaqueShapeC(val) : JS_ToCString(ctx, val));
    if (v) lxb_dom_element_set_attribute(el, (const lxb_char_t *)n, strlen(n), (const lxb_char_t *)v, strlen(v));
    if (v && (ex_str || !is_opq)) JS_FreeCString(ctx, v);   /* ToString'd frees; JS_OpaqueShapeC internal pointer does not */
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
    int is_opq = JS_IsOpaque(argv[1]);
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
    JSValue exv = is_opq ? JS_OpaqueExample(ctx, argv[1]) : JS_UNDEFINED;
    int ex_str = is_opq && !JS_IsUndefined(exv);
    const char *val = ex_str ? JS_ToCString(ctx, exv) : (is_opq ? JS_OpaqueShapeC(argv[1]) : JS_ToCString(ctx, argv[1]));
    if (val) lxb_dom_element_set_attribute(el, (const lxb_char_t *)name, strlen(name), (const lxb_char_t *)val, strlen(val));   /* baseline captured above (before the shadow write) */
    if (val && (ex_str || !is_opq)) JS_FreeCString(ctx, val);   /* ToString'd (example/concrete) frees; JS_OpaqueShapeC internal pointer does not */
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
JSValue js_el_rect(JSContext *ctx, JSValueConst t, int c, JSValueConst *v) {   /* getBoundingClientRect stub */
    JSValue o = JS_NewObject(ctx);
    const char *k[] = { "top", "left", "right", "bottom", "width", "height", "x", "y" };
    for (int i = 0; i < 8; i++) JS_SetPropertyStr(ctx, o, k[i], JS_NewInt32(ctx, 0));
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
            JSValue o = JS_NewOpaqueSourced(ctx, "{ssr}", "{ssr}");
            if (JS_IsOpaque(o)) { JS_SetOpaqueExample(ctx, o, r); return o; }   /* consumes r */
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
    if (argc > 0 && JS_IsOpaque(argv[0])) {
        const char *sh = JS_OpaqueShapeC(argv[0]);
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

/* Node::cloneNode -> a real node (self); its methods/attrs work. */
JSValue js_el_self(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) { (void)argc; (void)argv; return JS_DupValue(ctx, this_val); }

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
/* BEHAVIOR for the generated Element OPERATION shape (idl_bind matches by name; every other spec operation —
   scroll/requestFullscreen/getAttributeNode/... — installs as a present noop, never an undefined that throws).
   appendChild/insertBefore/replaceChild/before/after all route to the one tree-mutation + <script src> edge. */
static const IdlImpl ELEMENT_IMPLS[] = {
    { "getAttribute", NULL, NULL, js_el_getAttribute, -1 },
    { "setAttribute", NULL, NULL, js_el_setAttribute, -1 },
    { "getAttributeNames", NULL, NULL, js_el_getattrnames, -1 },
    { "hasAttribute", NULL, NULL, js_el_has_attr, -1 },
    { "toggleAttribute", NULL, NULL, js_el_has_attr, -1 },
    { "matches", NULL, NULL, js_el_matches, -1 },
    { "webkitMatchesSelector", NULL, NULL, js_el_matches, -1 },
    { "closest", NULL, NULL, js_el_closest, -1 },
    { "querySelector", NULL, NULL, js_el_querySelector, -1 },
    { "querySelectorAll", NULL, NULL, js_el_querySelectorAll, -1 },
    { "getElementsByTagName", NULL, NULL, js_el_querySelectorAll, -1 },
    { "insertAdjacentHTML", NULL, NULL, js_el_insertAdjacentHTML, -1 },
    { "attachShadow", NULL, NULL, js_el_attach_shadow, -1 },
    { "getBoundingClientRect", NULL, NULL, js_el_rect, -1 },
    { "appendChild", NULL, NULL, js_el_appendChild, -1 },
    { "insertBefore", NULL, NULL, js_el_appendChild, -1 },
    { "replaceChild", NULL, NULL, js_el_appendChild, -1 },
    { "before", NULL, NULL, js_el_appendChild, -1 },
    { "after", NULL, NULL, js_el_appendChild, -1 },
    { "append", NULL, NULL, js_el_appendChild, -1 },
    { "prepend", NULL, NULL, js_el_appendChild, -1 },
    { "cloneNode", NULL, NULL, js_el_self, -1 },
    { "contains", NULL, NULL, js_el_contains, -1 },
    { "addEventListener", NULL, NULL, js_add_listener, -1 },
};

void el_install_methods(JSContext *ctx, JSValue proto) {
    /* OPERATIONS: the member list comes from canonical Element IDL (Node + EventTarget + mixins, generated) —
       modelled ops get our impl, every other spec op is a present noop. Drift-free and complete. */
    idl_bind(ctx, proto, Element_IDL, Element_IDL_N, ELEMENT_IMPLS, (int)(sizeof ELEMENT_IMPLS / sizeof ELEMENT_IMPLS[0]), /*install_attrs*/0);
    /* Methods NOT on Element (HTMLElement.click/focus/blur, HTMLFormElement.submit) — kept until those interfaces
       are generated too; submit/requestSubmit fire the form's @H action request. */
    JS_SetPropertyStr(ctx, proto, "submit", JS_NewCFunction(ctx, js_form_submit, "submit", 0));
    JS_SetPropertyStr(ctx, proto, "requestSubmit", JS_NewCFunction(ctx, js_form_submit, "requestSubmit", 1));
    JS_SetPropertyStr(ctx, proto, "click", JS_NewCFunction(ctx, js_noop, "click", 0));
    JS_SetPropertyStr(ctx, proto, "focus", JS_NewCFunction(ctx, js_noop, "focus", 0));
    JS_SetPropertyStr(ctx, proto, "blur", JS_NewCFunction(ctx, js_noop, "blur", 0));
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
