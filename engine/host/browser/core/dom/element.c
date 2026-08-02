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
#include "core/dom/node.h"
#include "core/dom/document.h"

/* IDENTITY AND THE TREE BASE LIVE IN node.c — one wrapper table for every node kind, because a tree whose only
   node kind is Element cannot represent the document it just parsed. This file is what makes a node an ELEMENT:
   attributes, tagName, innerHTML, the reflected properties. */
static lxb_dom_element_t *elem_of(JSValueConst v)
{
    lxb_dom_node_t *n = node_of(v);
    if (!n || n->type != LXB_DOM_NODE_TYPE_ELEMENT) return NULL;
    return lxb_dom_interface_element(n);
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
        lxb_dom_node_t *frag;
        /* LEXBOR MUST NOT RUN PAGE CODE. That is what lets a parser — a state machine with a great deal of
           internal position — live inside an engine whose flows suspend and resume at any depth: the parse holds
           no continuation across anything the page can preempt, so it never has to be suspended and never has to
           be part of a snapshot. It completes inside one opcode over bytes, and any <script> it produces is
           QUEUED as a flow by element_prepare_script rather than executed by the tree builder.
           Re-entry is what a violation would look like: page code running mid-parse and reaching innerHTML
           again. Asserted here rather than assumed, because the day it stops holding is the day a half-built
           tree ends up inside another flow's delta. */
        static int in_parse;
        DCHECK(!in_parse, "a fragment parse re-entered itself — Lexbor ran page code mid-parse, and a parser "
                          "that holds a continuation across the page cannot live in a suspending engine: give "
                          "it the flow treatment or keep the script out of the tree builder");
        in_parse = 1;
        frag = lxb_html_document_parse_fragment(doc, el,
                                   (const lxb_char_t *)html, strlen(html));
        in_parse = 0;
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

/* §4.2.6 THE ParentNode MIXIN, on the interface the spec puts it on. querySelector/querySelectorAll were on
   Document only, so `section.querySelector("tbody")` — an ordinary scoped lookup, and where testharness.js
   stopped once elements became EventTargets — was "not a function". The selector engine is Document's ONE
   implementation, reached with this element as the root: a second copy here could disagree with it about what a
   selector means, which is the kind of divergence nothing would ever notice. */
static JSValue js_el_query_selector(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    lxb_dom_element_t *el = elem_of(this_val);
    const char *sel;
    JSValue r;

    if (!el) return magic ? JS_NewArray(ctx) : JS_NULL;
    if (argc < 1) return magic ? JS_NewArray(ctx) : JS_NULL;
    sel = JS_ToCString(ctx, argv[0]);
    if (!sel) return JS_EXCEPTION;
    r = document_qs_run(ctx, lxb_dom_interface_node(el), sel, magic);
    JS_FreeCString(ctx, sel);
    return r;
}

/* §4.2.6 children / firstElementChild / lastElementChild / childElementCount — the ELEMENT-only walk beside
   Node's childNodes, which a page uses precisely to skip the whitespace Text nodes a parser leaves behind.
   `children` is a STATIC array, the same named gap childNodes and querySelectorAll carry. */
static JSValue js_el_children(JSContext *ctx, JSValueConst this_val, int magic)
{
    lxb_dom_element_t *el = elem_of(this_val);
    lxb_dom_node_t *c, *first = NULL, *last = NULL;
    uint32_t n = 0;
    JSValue arr = JS_UNDEFINED;

    if (!el) return magic == 0 ? JS_NewArray(ctx) : (magic == 3 ? JS_NewInt32(ctx, 0) : JS_NULL);
    if (magic == 0) arr = JS_NewArray(ctx);
    for (c = lxb_dom_interface_node(el)->first_child; c; c = c->next) {
        if (c->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        if (!first) first = c;
        last = c;
        if (magic == 0) JS_SetPropertyUint32(ctx, arr, n, node_wrap(ctx, c));
        n++;
    }
    switch (magic) {
    case 0: return arr;
    case 1: return node_wrap(ctx, first);
    case 2: return node_wrap(ctx, last);
    default: return JS_NewInt32(ctx, (int)n);
    }
}

static const JSCFunctionListEntry js_element_proto[] = {
    JS_CFUNC_DEF("getAttribute", 1, js_el_get_attribute),
    JS_CFUNC_DEF("setAttribute", 2, js_el_set_attribute),
    JS_CFUNC_MAGIC_DEF("querySelector", 1, js_el_query_selector, 0),
    JS_CFUNC_MAGIC_DEF("querySelectorAll", 1, js_el_query_selector, 1),
    JS_CGETSET_MAGIC_DEF("children", js_el_children, NULL, 0),
    JS_CGETSET_MAGIC_DEF("firstElementChild", js_el_children, NULL, 1),
    JS_CGETSET_MAGIC_DEF("lastElementChild", js_el_children, NULL, 2),
    JS_CGETSET_MAGIC_DEF("childElementCount", js_el_children, NULL, 3),
    JS_CGETSET_DEF("tagName", js_el_get_tag, NULL),
    JS_CGETSET_DEF("innerHTML", NULL, js_el_set_inner_html),
    JS_CGETSET_DEF("textContent", js_el_get_text_content, js_el_set_text_content),
    JS_CGETSET_MAGIC_DEF("id", js_el_reflect_get, js_el_reflect_set, 0),
    JS_CGETSET_MAGIC_DEF("className", js_el_reflect_get, js_el_reflect_set, 1),
    JS_CGETSET_MAGIC_DEF("src", js_el_reflect_get, js_el_reflect_set, 2),
    JS_CGETSET_MAGIC_DEF("name", js_el_reflect_get, js_el_reflect_set, 3),
    JS_CGETSET_MAGIC_DEF("content", js_el_reflect_get, js_el_reflect_set, 4),
};


/* node.c calls this when it builds a wrapper for an ELEMENT node — it owns identity and the Node base; this
   owns what makes the node an Element. */
static void element_install_members(JSContext *ctx, JSValueConst obj)
{
    JS_SetPropertyFunctionList(ctx, (JSValue)obj, js_element_proto,
                               (int)(sizeof(js_element_proto) / sizeof(js_element_proto[0])));
}

/* HTML 4.12.1: a <script> that has just been inserted is PREPARED. node.c asks for this on every insertion so
   it does not have to know what a script is. */
static void element_on_inserted(JSContext *ctx, lxb_dom_node_t *n)
{
    if (n && n->type == LXB_DOM_NODE_TYPE_ELEMENT)
        element_prepare_script(ctx, lxb_dom_interface_element(n));
}

JSValue element_wrap(JSContext *ctx, lxb_dom_element_t *el)
{
    return node_wrap(ctx, el ? lxb_dom_interface_node(el) : NULL);
}

void element_init(JSContext *ctx)
{
    node_init(ctx);
    node_set_element_installer(element_install_members);
    node_set_inserted_hook(element_on_inserted);
}

void element_free(JSContext *ctx)
{
    node_free(ctx);
}
