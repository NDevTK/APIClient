/* THE ELEMENT INTERFACE — Blink core/dom, over the real Lexbor tree.
 *
 * IDENTITY IS THE INVARIANT. One JS object per Lexbor element, kept in a map on this component. A page compares
 * nodes by identity constantly (`el === document.body`, a Set of visited nodes, a WeakMap keyed by node), and a
 * fresh wrapper per lookup makes every one of those silently false — the page then re-walks, re-binds and
 * re-inserts, and this engine reports a surface built out of that confusion instead of the page's.
 *
 * READS are pure Lexbor and run no page code, so they are ordinary C. WRITES go through the solver's
 * chokepoints (dom_cow_set_attribute) because a DOM write is per-flow TIME-TRAVEL state: two forked arms write
 * the same attribute differently and each reads back its own. Never raw Lexbor, which would make the write
 * global and the flows visible to each other.
 *
 * setAttribute also carries TAINT. Lexbor stores bytes, so an attacker value written into an attribute and read
 * back would come out a plain string with its provenance gone; attr_shadow keeps the (element,name) -> concolic
 * association, so `el.setAttribute("data-x", location.hash)` followed later by a sink read is still solved. */
#include <stdlib.h>
#include <string.h>

#include <lexbor/html/html.h>

#include "check.h"
#include "quickjs.h"
#include "solver/attr_shadow.h"
#include "solver/concolic.h"
#include "solver/dom_cow.h"
#include "solver/endpoint.h"
#include "solver/engine.h"
#include "solver/solve.h"
#include "core/dom/element.h"

static JSClassID g_element_class;

typedef struct { lxb_dom_element_t *el; JSValue obj; } ElemEntry;
static ElemEntry *g_wraps;
static int        g_wrap_n, g_wrap_cap;

static lxb_dom_element_t *elem_of(JSValueConst v)
{
    return JS_GetOpaque(v, g_element_class);
}

static JSValue js_el_get_attribute(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    lxb_dom_element_t *el = elem_of(this_val);
    const char *name;
    JSValue r = JS_NULL;
    int si;

    if (!el || argc < 1) return JS_NULL;
    name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_EXCEPTION;
    /* The TAINT SHADOW answers first: an attacker value written here came back out of Lexbor as plain bytes
       with its provenance gone, and a sink reading it would look clean. */
    si = attr_shadow_find(el, ATTR_SLOT_ATTRIBUTE, name);
    if (si >= 0) {
        r = JS_DupValue(ctx, attr_shadow_opaque(si));   /* borrowed — dup to hand it out */
    } else {
        size_t vl = 0;
        const lxb_char_t *v = lxb_dom_element_get_attribute(el, (const lxb_char_t *)name, strlen(name), &vl);
        if (v) r = JS_NewStringLen(ctx, (const char *)v, vl);
    }
    JS_FreeCString(ctx, name);
    return r;
}

static JSValue js_el_set_attribute(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    lxb_dom_element_t *el = elem_of(this_val);
    const char *name;
    const char *val;

    if (!el || argc < 2) return JS_UNDEFINED;
    name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_EXCEPTION;
    /* A concolic value has no bytes to store. Record it in the shadow so the read gives the SAME concolic back,
       and write its shape into the tree so a serialization of the document still shows something. */
    if (concolic_is(argv[1])) {
        attr_shadow_set(ctx, el, ATTR_SLOT_ATTRIBUTE, name, argv[1]);
        /* NEVER ToString a concolic to get bytes: its coercion belongs to the concolic hooks and answers with
           another concolic, so the call THROWS — and the throw was being dropped here, leaving a pending
           exception behind a normal return. The SHAPE is the honest byte form for the tree. */
        val = concolic_shape_c(argv[1]);
        dom_cow_set_attribute(el, name, val ? val : "", val ? strlen(val) : 0);
        JS_FreeCString(ctx, name);
        return JS_UNDEFINED;
    }
    attr_shadow_set(ctx, el, ATTR_SLOT_ATTRIBUTE, name, JS_UNDEFINED);   /* a concrete write clears any earlier taint */
    val = JS_ToCString(ctx, argv[1]);
    if (!val) { JS_FreeCString(ctx, name); return JS_EXCEPTION; }
    dom_cow_set_attribute(el, name, val, strlen(val));   /* chokepoint: capture-then-mutate, per flow */
    JS_FreeCString(ctx, val);
    JS_FreeCString(ctx, name);
    return JS_UNDEFINED;
}

static JSValue js_el_get_tag(JSContext *ctx, JSValueConst this_val)
{
    lxb_dom_element_t *el = elem_of(this_val);
    size_t n = 0;
    const lxb_char_t *t;
    if (!el) return JS_UNDEFINED;
    t = lxb_dom_element_qualified_name(el, &n);
    return t ? JS_NewStringLen(ctx, (const char *)t, n) : JS_UNDEFINED;
}

/* innerHTML= is TWO things and it must do both.
   It is an HTML-CONTEXT SINK, so the assigned value goes to the solver, which decides the breakout against the
   real parse context. And it REPLACES this element's children with the parsed markup — a page that builds its
   DOM this way and then queries it must find what it built, or every getElementById after it answers null and
   the engine reports a surface the page never had. Reporting the sink and dropping the markup was the second
   half missing.
   Both halves go through the per-flow chokepoints: the old children are removed with a capture so they come
   back on a context switch, and each parsed node is inserted with one, so two forked arms each see their own
   subtree. A concolic value has no bytes to parse — the sink report IS the answer for it. */
static JSValue js_el_set_inner_html(JSContext *ctx, JSValueConst this_val, JSValueConst val)
{
    lxb_dom_element_t *el = elem_of(this_val);
    const char *html;
    lxb_dom_node_t *node, *next;

    if (!el) return JS_UNDEFINED;
    solve_html_sink(ctx, val);
    if (concolic_is(val))
        return JS_UNDEFINED;   /* nothing concrete to parse; the sink is what this write means */

    html = JS_ToCString(ctx, val);
    if (!html) return JS_EXCEPTION;

    for (node = lxb_dom_interface_node(el)->first_child; node; node = next) {
        next = node->next;
        dom_cow_remove_child(node);
    }
    {
        lxb_html_document_t *doc = lxb_html_interface_document(lxb_dom_interface_node(el)->owner_document);
        lxb_dom_node_t *frag = lxb_html_document_parse_fragment(doc, el,
                                   (const lxb_char_t *)html, strlen(html));
        if (frag) {
            for (node = frag->first_child; node; node = next) {
                next = node->next;
                lxb_dom_node_remove(node);              /* out of the fragment, which nothing else observes */
                dom_cow_append_child(lxb_dom_interface_node(el), node);   /* into the tree, per flow */
            }
            lxb_dom_node_destroy(frag);
        }
    }
    JS_FreeCString(ctx, html);
    return JS_UNDEFINED;
}

/* 4.4 Node.textContent. NOT a markup sink — the setter creates ONE Text node and the getter concatenates
   descendant text — which is exactly why a page that has been told to stop using innerHTML uses it, and why an
   engine that lacked it saw those pages build nothing. The taint travels the same way an attribute's does: the
   assigned concolic is recorded on the element's property slot, so a source parked in the DOM as text and later
   read back into a real sink is still solved. */
static JSValue js_el_get_text_content(JSContext *ctx, JSValueConst this_val)
{
    lxb_dom_element_t *el = elem_of(this_val);
    lxb_char_t *txt;
    size_t len = 0;
    JSValue r;
    int si;

    if (!el) return JS_NULL;
    si = attr_shadow_find(el, ATTR_SLOT_PROPERTY, "textContent");
    if (si >= 0)
        return JS_DupValue(ctx, attr_shadow_opaque(si));
    txt = lxb_dom_node_text_content(lxb_dom_interface_node(el), &len);
    if (!txt) return JS_NewStringLen(ctx, "", 0);
    r = JS_NewStringLen(ctx, (const char *)txt, len);
    lxb_dom_document_destroy_text(lxb_dom_interface_node(el)->owner_document, txt);
    return r;
}

static JSValue js_el_set_text_content(JSContext *ctx, JSValueConst this_val, JSValueConst val)
{
    lxb_dom_element_t *el = elem_of(this_val);
    lxb_dom_node_t *node, *next, *n = lxb_dom_interface_node(el);
    lxb_dom_text_t *text;
    const char *str;
    size_t len;

    if (!el) return JS_UNDEFINED;
    dom_cow_set_prop_taint(ctx, el, "textContent", concolic_is(val) ? val : JS_UNDEFINED);
    /* A concolic has no bytes: its coercion is the concolic hooks' and throws rather than producing a string,
       so the SHAPE is what the Text node carries while the shadow carries the value. */
    if (concolic_is(val)) {
        str = concolic_shape_c(val);
        len = str ? strlen(str) : 0;
        if (!str) str = "";
    } else {
        str = JS_ToCStringLen(ctx, &len, val);
        if (!str) return JS_EXCEPTION;
    }
    /* "string replace all": every child goes, then ONE Text node — both through the per-flow chokepoints, so a
       forked arm that sets different text reads back its own. */
    for (node = n->first_child; node; node = next) {
        next = node->next;
        dom_cow_remove_child(node);
    }
    text = lxb_dom_document_create_text_node(n->owner_document, (const lxb_char_t *)str, len);
    DCHECK(text != NULL, "textContent= produced no Text node — the page's text would silently not be there");
    if (text)
        dom_cow_append_child(n, lxb_dom_interface_node(text));
    if (!concolic_is(val))
        JS_FreeCString(ctx, str);   /* the shape is the concolic's own storage, not a C string to release */
    return JS_UNDEFINED;
}

/* [Reflect]ed content attributes: the IDL property IS the attribute, so both directions go through the same
   attribute read (taint shadow first) and the same per-flow write. The MAGIC IS AN INDEX INTO THIS TABLE and
   nothing else — a boolean magic was fine for two and became a lie at three. Spelling the list out rather than
   generating it from the IDL is the gap engine/idlgen.mjs exists to report; what must not happen is a property
   that answers something its attribute does not say, which is exactly what `script.src` did: with no reflection
   it became an ordinary JS property, the element carried no src attribute, and the injected script named a URL
   nothing would ever fetch. */
static const char *const EL_REFLECT[] = { "id", "class", "src", "name", "content" };

static JSValue js_el_reflect_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    JSValue nv, r;
    DCHECK(magic >= 0 && magic < (int)(sizeof(EL_REFLECT) / sizeof(EL_REFLECT[0])),
           "a reflected property was declared with a magic the attribute table does not name");
    nv = JS_NewString(ctx, EL_REFLECT[magic]);
    r = js_el_get_attribute(ctx, this_val, 1, (JSValueConst *)&nv);
    JS_FreeValue(ctx, nv);
    return JS_IsNull(r) ? JS_NewStringLen(ctx, "", 0) : r;   /* a reflected string attribute defaults to "" */
}

static JSValue js_el_reflect_set(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic)
{
    JSValue args[2];
    JSValue r;
    DCHECK(magic >= 0 && magic < (int)(sizeof(EL_REFLECT) / sizeof(EL_REFLECT[0])),
           "a reflected property was declared with a magic the attribute table does not name");
    args[0] = JS_NewString(ctx, EL_REFLECT[magic]);
    args[1] = JS_DupValue(ctx, val);
    r = js_el_set_attribute(ctx, this_val, 2, (JSValueConst *)args);
    JS_FreeValue(ctx, args[0]);
    JS_FreeValue(ctx, args[1]);
    return r;
}

/* 4.12.1 "prepare the script", the insertion half. A page loads code conditionally in three ways and this is the
   second: `s = createElement("script"); s.src = u; body.appendChild(s)`. Before this the injection was a SILENT
   no-op — the element went into the tree and the code it named was never fetched, never run, never even
   reported, so every endpoint and sink behind an A/B flag or a feature gate was missing with nothing to say so.
   The loaded code is more PROGRAM OF THE INJECTING FLOW: it joins that flow's script sequence, so it runs under
   the delta, the pins and the position in the BFS of the world that injected it, and a sibling that never took
   the branch never sees it.
   INSERTION is the trigger, which is why this is here and not in the innerHTML path: markup parsed into
   innerHTML does not execute its scripts, and that difference is load-bearing for the @S breakout contexts. */
static void element_prepare_script(JSContext *ctx, lxb_dom_element_t *el)
{
    size_t n = 0, vl = 0;
    const lxb_char_t *tag = lxb_dom_element_qualified_name(el, &n);
    const lxb_char_t *src;
    int si;

    if (!tag || n != 6 || memcmp(tag, "script", 6) != 0)
        return;
    /* An UNKNOWN src is a URL this engine cannot fetch, but it is still a request the page makes — recorded so
       it reaches the @H surface as the shape it is, rather than disappearing. */
    si = attr_shadow_find(el, ATTR_SLOT_ATTRIBUTE, "src");
    if (si >= 0) {
        endpoint_record(ctx, "GET", attr_shadow_opaque(si));
        return;
    }
    src = lxb_dom_element_get_attribute(el, (const lxb_char_t *)"src", 3, &vl);
    if (src && vl) {
        char *u = malloc(vl + 1);
        CHECK(u, "element: OOM copying an injected script's URL");
        memcpy(u, src, vl); u[vl] = 0;
        engine_pending_script_url(ctx, u);
        free(u);
        return;
    }
    /* No src: the element's own text IS the program, and it runs on insertion. */
    {
        lxb_char_t *txt = lxb_dom_node_text_content(lxb_dom_interface_node(el), &n);
        if (txt) {
            if (n) engine_queue_script((const char *)txt);
            lxb_dom_document_destroy_text(lxb_dom_interface_node(el)->owner_document, txt);
        }
    }
}

/* 4.2.3 appendChild / removeChild. The tree mutation a page performs after createElement — without them a built
   subtree never joined the document and every query after it answered null. Both are the per-flow chokepoints,
   and both return the node the spec returns (pages chain on it: `p.appendChild(c).setAttribute(...)`). */
static JSValue js_el_append_child(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    lxb_dom_element_t *el = elem_of(this_val), *child;

    if (!el || argc < 1) return JS_UNDEFINED;
    child = elem_of(argv[0]);
    DCHECK(child != NULL, "appendChild was given something that is not an element wrapper — a Text/Comment node "
                          "has no wrapper class yet, and inserting nothing would leave the page's tree short "
                          "of what it built");
    if (!child) return JS_UNDEFINED;
    dom_cow_append_child(lxb_dom_interface_node(el), lxb_dom_interface_node(child));
    element_prepare_script(ctx, child);
    return JS_DupValue(ctx, argv[0]);
}

static JSValue js_el_remove_child(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    lxb_dom_element_t *el = elem_of(this_val), *child;

    if (!el || argc < 1) return JS_UNDEFINED;
    child = elem_of(argv[0]);
    DCHECK(child != NULL, "removeChild was given something that is not an element wrapper");
    if (!child) return JS_UNDEFINED;
    DCHECK(lxb_dom_interface_node(child)->parent == lxb_dom_interface_node(el),
           "removeChild was given a node that is not a child of this element — the spec throws NotFoundError "
           "there, and this engine has no DOMException to throw yet");
    dom_cow_remove_child(lxb_dom_interface_node(child));
    return JS_DupValue(ctx, argv[0]);
}

static const JSCFunctionListEntry js_element_proto[] = {
    JS_CFUNC_DEF("getAttribute", 1, js_el_get_attribute),
    JS_CFUNC_DEF("setAttribute", 2, js_el_set_attribute),
    JS_CFUNC_DEF("appendChild", 1, js_el_append_child),
    JS_CFUNC_DEF("removeChild", 1, js_el_remove_child),
    JS_CGETSET_DEF("tagName", js_el_get_tag, NULL),
    JS_CGETSET_DEF("innerHTML", NULL, js_el_set_inner_html),
    JS_CGETSET_DEF("textContent", js_el_get_text_content, js_el_set_text_content),
    JS_CGETSET_MAGIC_DEF("id", js_el_reflect_get, js_el_reflect_set, 0),
    JS_CGETSET_MAGIC_DEF("className", js_el_reflect_get, js_el_reflect_set, 1),
    JS_CGETSET_MAGIC_DEF("src", js_el_reflect_get, js_el_reflect_set, 2),
    JS_CGETSET_MAGIC_DEF("name", js_el_reflect_get, js_el_reflect_set, 3),
    JS_CGETSET_MAGIC_DEF("content", js_el_reflect_get, js_el_reflect_set, 4),
};

static void element_finalizer(JSRuntime *rt, JSValue val) { (void)rt; (void)val; }

JSValue element_wrap(JSContext *ctx, lxb_dom_element_t *el)
{
    JSValue obj;
    int i;

    if (!el)
        return JS_NULL;
    for (i = 0; i < g_wrap_n; i++)
        if (g_wraps[i].el == el)
            return JS_DupValue(ctx, g_wraps[i].obj);

    obj = JS_NewObjectClass(ctx, g_element_class);
    if (JS_IsException(obj))
        return obj;
    JS_SetOpaque(obj, el);
    JS_SetPropertyFunctionList(ctx, obj, js_element_proto,
                               (int)(sizeof(js_element_proto) / sizeof(js_element_proto[0])));

    if (g_wrap_n == g_wrap_cap) {
        int c = g_wrap_cap ? g_wrap_cap * 2 : 16;
        ElemEntry *a = realloc(g_wraps, sizeof(*a) * (size_t)c);
        CHECK(a != NULL, "the element wrapper table allocation failed: a dropped wrapper breaks node identity, "
                         "and every `el === other` the page makes after it is silently false");
        g_wraps = a; g_wrap_cap = c;
    }
    g_wraps[g_wrap_n].el = el;
    g_wraps[g_wrap_n].obj = JS_DupValue(ctx, obj);
    g_wrap_n++;
    return obj;
}

void element_init(JSContext *ctx)
{
    JSClassDef def = { "Element", .finalizer = element_finalizer };
    JS_NewClassID(JS_GetRuntime(ctx), &g_element_class);
    JS_NewClass(JS_GetRuntime(ctx), g_element_class, &def);
}

void element_free(JSContext *ctx)
{
    int i;
    for (i = 0; i < g_wrap_n; i++)
        JS_FreeValue(ctx, g_wraps[i].obj);
    free(g_wraps);
    g_wraps = NULL; g_wrap_n = g_wrap_cap = 0;
}
