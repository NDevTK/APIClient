/* DOM Element JSClass + el_wrap — see dom_element.h.
 *
 * The wrapper is deliberately LOGIC-FREE: it only binds a real Lexbor node to a JS Element object whose
 * methods (still in main.c, migrating here) are DOM host-edges the ONE BFS scheduler drives — el_wrap adds
 * no control flow of its own, so no logic ever lives outside the scheduler. */
#include <string.h>
#include <lexbor/html/html.h>   /* lxb_html_template_element_t for <template>.content */
#include "dom_element.h"
#include "dom_select.h"   /* dom_node_matches — matches()/closest() run the real CSS selector */
#include "solve.h"        /* solve_add — a reflected href/src/srcdoc property SET is an @S sink */
#include "dom_cow.h"      /* dom_attr_capture — a reflected-property write joins the per-flow COW delta */
#include "attr_shadow.h"  /* attr_shadow_set — a concolic value set via property keeps its taint+example */

/* Borrowed from main.c: an @S replay flow pins the concrete candidate here (a reflected-property write in a
   replay writes the candidate, not the shadow taint). */
extern char *g_candidate;

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
JSValue js_el_style_get(JSContext *ctx, JSValueConst this_val) { return JS_NewObject(ctx); }
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
