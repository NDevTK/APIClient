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
#include "core/idl_args.h"
#include "core/events/event_target.h"
#include "core/html/html_element.h"
#include <lexbor/ns/ns.h>

/* The two shapes every DOM member in this file has. Spelled once so a member declares its IDL, not a bitmask. */
static const IdlArgType IDL_1STR[1] = { IDL_DOMSTRING };
static const IdlArgType IDL_2STR[2] = { IDL_DOMSTRING, IDL_DOMSTRING };
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

static JSValue js_el_get_attribute(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    (void)magic;
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

static JSValue js_el_set_attribute(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    (void)magic;
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
static JSValue js_el_set_inner_html(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic)
{
    lxb_dom_element_t *el = elem_of(this_val);
    const char *html;
    lxb_dom_node_t *node, *next;

    (void)magic;
    if (!el) return JS_UNDEFINED;
    solve_html_sink(ctx, val);
    if (concolic_is(val))
        return JS_UNDEFINED;   /* nothing concrete to parse; the sink is what this write means */

    DCHECK(JS_IsString(val), "innerHTML= reached the body unconverted — the IDL declaration is what converts "
                             "it, and running the page's toString from here is the drive-to-completion the flow "
                             "machinery exists to avoid");
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

/* §4.9 removeAttribute / hasAttribute / toggleAttribute / hasAttributes / getAttributeNames — the rest of the
   attribute family. removeAttribute in particular had no implementation at all, so a boolean reflection had no
   way to UNSET itself and `el.hidden = false` could only ever set. magic: 0 remove, 1 has, 2 toggle. */
static JSValue js_el_attr_op(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    lxb_dom_element_t *el = elem_of(this_val);
    const char *name;
    size_t vl = 0;
    bool present;
    JSValue r;

    if (!el || argc < 1) return magic == 0 ? JS_UNDEFINED : JS_FALSE;
    name = JS_ToCString(ctx, argv[0]);   /* a real string by now: the declaration converted it */
    if (!name) return JS_EXCEPTION;
    present = lxb_dom_element_get_attribute(el, (const lxb_char_t *)name, strlen(name), &vl) != NULL;
    switch (magic) {
    case 0:
        dom_cow_remove_attribute(el, name);
        attr_shadow_set(ctx, el, ATTR_SLOT_ATTRIBUTE, name, JS_UNDEFINED);   /* the taint goes with the value */
        r = JS_UNDEFINED;
        break;
    case 1:
        r = JS_NewBool(ctx, present);
        break;
    default:
        /* §4.9 toggleAttribute(name, optional force): with no force it flips; with one it sets or removes. */
        DCHECK(magic == 2, "an attribute operation was declared with a magic this file does not name");
        {
            bool want = (argc > 1 && !JS_IsUndefined(argv[1])) ? JS_ToBool(ctx, argv[1]) : !present;
            if (want) dom_cow_set_attribute(el, name, "", 0);
            else {
                dom_cow_remove_attribute(el, name);
                attr_shadow_set(ctx, el, ATTR_SLOT_ATTRIBUTE, name, JS_UNDEFINED);
            }
            r = JS_NewBool(ctx, want);
        }
        break;
    }
    JS_FreeCString(ctx, name);
    return r;
}

/* §4.9 hasAttributes / getAttributeNames / localName / prefix / namespaceURI — pure reads over the attribute
   list Lexbor already holds. magic: 0 hasAttributes, 1 getAttributeNames. */
static JSValue js_el_attr_list(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    lxb_dom_element_t *el = elem_of(this_val);
    lxb_dom_attr_t *at;
    JSValue arr;
    uint32_t i = 0;

    (void)argc; (void)argv;
    if (!el) return magic == 0 ? JS_FALSE : JS_NewArray(ctx);
    if (magic == 0)
        return JS_NewBool(ctx, el->first_attr != NULL);
    DCHECK(magic == 1, "an attribute-list read was declared with a magic this file does not name");
    arr = JS_NewArray(ctx);
    for (at = el->first_attr; at; at = at->next) {
        size_t n = 0;
        const lxb_char_t *k = lxb_dom_attr_qualified_name(at, &n);
        if (k) JS_SetPropertyUint32(ctx, arr, i++, JS_NewStringLen(ctx, (const char *)k, n));
    }
    return arr;
}

/* §4.9 localName / prefix / namespaceURI — the three parts of an element's NAME the spec keeps apart, and each
   of which Lexbor already interned. magic: 0 localName, 1 prefix, 2 namespaceURI. */
static JSValue js_el_name_part(JSContext *ctx, JSValueConst this_val, int magic)
{
    lxb_dom_element_t *el = elem_of(this_val);
    size_t n = 0;
    const lxb_char_t *v;

    if (!el) return JS_NULL;
    switch (magic) {
    case 0: v = lxb_dom_element_local_name(el, &n); break;
    case 1: v = lxb_dom_element_prefix(el, &n);     break;
    default:
        DCHECK(magic == 2, "an element name part was declared with a magic this file does not name");
        v = lxb_ns_by_id(lxb_dom_interface_node(el)->owner_document->ns,
                         lxb_dom_interface_node(el)->ns, &n);
        break;
    }
    if (!v || !n) return magic == 0 ? JS_NewStringLen(ctx, "", 0) : JS_NULL;
    return JS_NewStringLen(ctx, (const char *)v, n);
}

/* [Reflect]ed content attributes: the IDL property IS the attribute, so both directions go through the same
   attribute read (taint shadow first) and the same per-flow write. Spelling the list out rather than generating
   it from the IDL is the gap engine/idlgen.mjs exists to report; what must not happen is a property that answers
   something its attribute does not say, which is exactly what `script.src` did: with no reflection it became an
   ordinary JS property, the element carried no src attribute, and the injected script named a URL nothing would
   ever fetch.
   THE IDL NAME AND THE CONTENT-ATTRIBUTE NAME ARE TWO DIFFERENT STRINGS and the pair is what a reflection is —
   `className` reflects `class`, `htmlFor` reflects `for`, `httpEquiv` reflects `http-equiv`.
   THE TABLE IS NO LONGER ONE FLAT LIST ON Element, because the IDL does not put them there: `src` belongs to
   HTMLScriptElement, HTMLImageElement and HTMLIFrameElement, `content` to HTMLMetaElement, `name` to a dozen
   interfaces. An interface DECLARES its own reflections and hands them to element_install_reflections, which
   assigns each a magic out of one shared registry — so the magic is still an index into one table and the
   bodies below still take exactly one. */
static ElReflect g_reflect[320];
static int       g_reflect_n;

static JSValue js_el_reflect_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    lxb_dom_element_t *el = elem_of(this_val);
    JSValue nv, r;
    size_t vl = 0;

    DCHECK(magic >= 0 && magic < g_reflect_n,
           "a reflected property was declared with a magic the registry does not name");
    if (!el) return g_reflect[magic].kind == REFLECT_BOOL ? JS_FALSE : JS_NewStringLen(ctx, "", 0);
    /* §2.2.1 a BOOLEAN reflection is the attribute's PRESENCE, not its value — `<input disabled>` and
       `<input disabled="false">` are both disabled, and a string reflection here would report "false". */
    if (g_reflect[magic].kind == REFLECT_BOOL)
        return JS_NewBool(ctx, lxb_dom_element_get_attribute(el, (const lxb_char_t *)g_reflect[magic].attr,
                                                             strlen(g_reflect[magic].attr), &vl) != NULL);
    nv = JS_NewString(ctx, g_reflect[magic].attr);
    r = js_el_get_attribute(ctx, this_val, 1, (JSValueConst *)&nv, 0);   /* a real string already: the reflected NAME is the engine's */
    JS_FreeValue(ctx, nv);
    return JS_IsNull(r) ? JS_NewStringLen(ctx, "", 0) : r;   /* a reflected string attribute defaults to "" */
}

static JSValue js_el_reflect_set(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic)
{
    lxb_dom_element_t *el = elem_of(this_val);
    JSValue args[2];
    JSValue r;

    DCHECK(magic >= 0 && magic < g_reflect_n,
           "a reflected property was declared with a magic the registry does not name");
    if (g_reflect[magic].kind == REFLECT_BOOL) {
        if (!el) return JS_UNDEFINED;
        /* §2.2.1: setting true ADDS the attribute with the empty string, setting false REMOVES it. */
        if (JS_ToBool(ctx, val)) dom_cow_set_attribute(el, g_reflect[magic].attr, "", 0);
        else {
            dom_cow_remove_attribute(el, g_reflect[magic].attr);
            attr_shadow_set(ctx, el, ATTR_SLOT_ATTRIBUTE, g_reflect[magic].attr, JS_UNDEFINED);
        }
        return JS_UNDEFINED;
    }
    args[0] = JS_NewString(ctx, g_reflect[magic].attr);
    args[1] = JS_DupValue(ctx, val);
    r = js_el_set_attribute(ctx, this_val, 2, (JSValueConst *)args, 0);
    JS_FreeValue(ctx, args[0]);
    JS_FreeValue(ctx, args[1]);
    return r;
}

void element_install_reflections(JSContext *ctx, JSValueConst proto, const ElReflect *r, int n)
{
    int i;

    for (i = 0; i < n; i++) {
        CHECK(g_reflect_n < (int)(sizeof(g_reflect) / sizeof(g_reflect[0])),
              "the reflection registry is full — raise it rather than dropping an interface's attributes");
        g_reflect[g_reflect_n] = r[i];
        /* A boolean reflection's value is ToBoolean, which is total and runs none of the page's code; a string
           one is a DOMString, which is ToString on whatever the page passed. Two types, one declaration each. */
        idl_install_accessor(ctx, proto, r[i].idl, js_el_reflect_get, g_reflect_n,
                             idl_setter_id(ctx, r[i].kind == REFLECT_BOOL ? IDL_ANY : IDL_DOMSTRING,
                                           false, js_el_reflect_set, g_reflect_n));
        g_reflect_n++;
    }
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

/* The READ-ONLY members: pure walks over the flow's own tree, so they are ordinary C getters. */
/* §4.9 the three parts of an element's name, each a pure Lexbor read. */
static const JSCFunctionListEntry js_element_name_parts[] = {
    JS_CGETSET_MAGIC_DEF("localName", js_el_name_part, NULL, 0),
    JS_CGETSET_MAGIC_DEF("prefix", js_el_name_part, NULL, 1),
    JS_CGETSET_MAGIC_DEF("namespaceURI", js_el_name_part, NULL, 2),
};

static const JSCFunctionListEntry js_element_readonly[] = {
    JS_CGETSET_MAGIC_DEF("children", js_el_children, NULL, 0),
    JS_CGETSET_MAGIC_DEF("firstElementChild", js_el_children, NULL, 1),
    JS_CGETSET_MAGIC_DEF("lastElementChild", js_el_children, NULL, 2),
    JS_CGETSET_MAGIC_DEF("childElementCount", js_el_children, NULL, 3),
    JS_CGETSET_DEF("tagName", js_el_get_tag, NULL),
};

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

/* ELEMENT.PROTOTYPE — §4.9, `interface Element : Node`, built ONCE on top of the base node.c owns and handed to
   node.c as the interface every element node wears. It used to be a per-wrapper INSTALLER callback, which minted
   a fresh closure for every member of every element and made `a.getAttribute === b.getAttribute` false. */
static JSValue g_element_proto;

void element_init(JSContext *ctx)
{
    JSValue proto;

    node_init(ctx);

    /* `name`, `value` and `selectors` are DOMStrings, so each is ToString on whatever the page passed:
       `el.getAttribute({toString(){ … }})` is the page's code, and the declaration parks the machine on that
       argument rather than running it out of a C activation. */
    proto = JS_NewObjectProto(ctx, node_proto());
    CHECK(!JS_IsException(proto), "Element.prototype could not be allocated");
    JS_SetPropertyFunctionList(ctx, proto, js_element_readonly,
                               (int)(sizeof(js_element_readonly) / sizeof(js_element_readonly[0])));
    idl_install_method(ctx, proto, "getAttribute", 1,
                       idl_method_id(ctx, IDL_1STR, 1, js_el_get_attribute, 0));
    idl_install_method(ctx, proto, "setAttribute", 2,
                       idl_method_id(ctx, IDL_2STR, 2, js_el_set_attribute, 0));
    idl_install_method(ctx, proto, "querySelector", 1,
                       idl_method_id(ctx, IDL_1STR, 1, js_el_query_selector, 0));
    idl_install_method(ctx, proto, "querySelectorAll", 1,
                       idl_method_id(ctx, IDL_1STR, 1, js_el_query_selector, 1));

    /* `[CEReactions] attribute [LegacyNullToEmptyString] DOMString innerHTML` — the extended attribute is part
       of the TYPE, so `el.innerHTML = null` empties the element instead of parsing the markup `null`. WRITE-ONLY
       here: the serialising getter is an HTML fragment serialiser this engine does not have yet, and answering
       undefined is the honest absence the IDL audit names. */
    idl_install_accessor(ctx, proto, "innerHTML", NULL, 0,
                         idl_setter_id(ctx, IDL_DOMSTRING, true, js_el_set_inner_html, 0));

    /* The rest of the attribute family. removeAttribute had no implementation at all, which is also why a
       boolean reflection could not unset itself. */
    idl_install_method(ctx, proto, "removeAttribute", 1,
                       idl_method_id(ctx, IDL_1STR, 1, js_el_attr_op, 0));
    idl_install_method(ctx, proto, "hasAttribute", 1,
                       idl_method_id(ctx, IDL_1STR, 1, js_el_attr_op, 1));
    {
        static const IdlArgType TOGGLE[2] = { IDL_DOMSTRING, IDL_ANY };   /* `optional boolean force` is ToBoolean */
        idl_install_method(ctx, proto, "toggleAttribute", 1,
                           idl_method_id(ctx, TOGGLE, 2, js_el_attr_op, 2));
    }
    JS_SetPropertyStr(ctx, proto, "hasAttributes",
                      JS_NewCFunctionMagic(ctx, js_el_attr_list, "hasAttributes", 0, JS_CFUNC_generic_magic, 0));
    JS_SetPropertyStr(ctx, proto, "getAttributeNames",
                      JS_NewCFunctionMagic(ctx, js_el_attr_list, "getAttributeNames", 0, JS_CFUNC_generic_magic, 1));
    JS_SetPropertyFunctionList(ctx, proto, js_element_name_parts,
                               (int)(sizeof(js_element_name_parts) / sizeof(js_element_name_parts[0])));

    /* §4.9's OWN reflections, and only those: `id`, `class` and `slot`. `src`, `name` and `content` used to be
       here too, which is three properties Element's IDL does not declare — they belong to HTMLScriptElement,
       to a dozen form interfaces and to HTMLMetaElement, and they are installed there now. */
    {
        static const ElReflect R[] = {
            { "id", "id", REFLECT_STRING }, { "className", "class", REFLECT_STRING },
            { "slot", "slot", REFLECT_STRING },
        };
        element_install_reflections(ctx, proto, R, (int)(sizeof(R) / sizeof(R[0])));
    }
    /* GlobalEventHandlers is NOT on Element — the IDL mixes it into HTMLElement, which is where it is installed
       now that that interface exists. */
    g_element_proto = proto;
    node_set_proto(ctx, LXB_DOM_NODE_TYPE_ELEMENT, JS_DupValue(ctx, proto));
    node_set_inserted_hook(element_on_inserted);
    html_element_init(ctx);   /* the HTML half, which builds HTMLElement and the per-tag interfaces on this */
}

JSValueConst element_proto(void) { DCHECK(JS_IsObject(g_element_proto), "Element.prototype was asked for before element_init built it"); return g_element_proto; }

void element_free(JSContext *ctx)
{
    html_element_free(ctx);
    JS_FreeValue(ctx, g_element_proto);
    g_element_proto = JS_UNDEFINED;
    g_reflect_n = 0;
    node_free(ctx);
}
