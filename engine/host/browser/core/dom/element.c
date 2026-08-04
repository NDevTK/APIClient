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
#include "core/html/custom_elements.h"
#include "core/dom/dom_token_list.h"
#include "core/dom/collections.h"
#include "core/idl_indexed.h"
#include "core/css/css_style_declaration.h"
#include <lexbor/ns/ns.h>

/* The two shapes every DOM member in this file has. Spelled once so a member declares its IDL, not a bitmask. */
static const IdlArgType IDL_1STR[1] = { IDL_DOMSTRING };
static const IdlArgType IDL_2STR[2] = { IDL_DOMSTRING, IDL_DOMSTRING };
#include <lexbor/html/serialize.h>
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

/* §8.4 THE FRAGMENT SERIALISER — innerHTML and outerHTML as READS, which they were not: the accessor was
   write-only and every `el.innerHTML` answered undefined. That is the worst shape a gap can take here, because
   undefined does not throw — it PROPAGATES. `wrap.innerHTML = head.innerHTML + row` builds the string
   "undefined…" and the page carries on, so the engine reports a surface assembled out of a value the page
   never had, and nothing anywhere names the missing capability.
   Lexbor owns the serialisation, which is the point: the algorithm is HTML's own (void elements, the raw-text
   ones, attribute escaping, `<template>`'s content), and hand-rolling it here would be a second HTML
   serialiser that disagrees with the parser sitting beside it.
   magic 0 = innerHTML (the CHILDREN), 1 = outerHTML (the element itself). */
static JSValue js_el_get_html(JSContext *ctx, JSValueConst this_val, int magic)
{
    lxb_dom_element_t *el = elem_of(this_val);
    lxb_dom_node_t *n;
    lexbor_str_t str = { 0 };
    JSValue r;

    if (!el) return JS_UNDEFINED;
    n = lxb_dom_interface_node(el);
    if (lexbor_str_init(&str, n->owner_document->text, 64) == NULL)
        return JS_ThrowOutOfMemory(ctx);
    if (magic == 0) {
        lxb_dom_node_t *c;
        for (c = n->first_child; c; c = c->next)
            if (lxb_html_serialize_tree_str(c, &str) != LXB_STATUS_OK) break;
    } else {
        lxb_html_serialize_tree_str(n, &str);
    }
    r = JS_NewStringLen(ctx, (const char *)str.data, str.length);
    lexbor_str_destroy(&str, n->owner_document->text, false);
    return r;
}

/* §13.4 THE FRAGMENT PARSE, as ONE operation, because there are four members that do it and they differ only
   in where the result goes. `context` is the element whose parsing state the fragment is parsed IN — a `<tr>`
   is dropped anywhere but inside a table, and that is the tree builder's rule, not something a caller chooses.
   The parsed nodes are handed to `place`, which inserts each through the per-flow chokepoints.
   LEXBOR MUST NOT RUN PAGE CODE. That is what lets a parser — a state machine with a great deal of internal
   position — live inside an engine whose flows suspend and resume at any depth: the parse holds no continuation
   across anything the page can preempt, so it never has to be suspended and never has to be part of a snapshot.
   It completes inside one opcode over bytes, and any <script> it produces is QUEUED as a flow by
   element_prepare_script rather than executed by the tree builder.
   Re-entry is what a violation would look like: page code running mid-parse and reaching one of these again.
   Asserted rather than assumed, because the day it stops holding is the day a half-built tree ends up inside
   another flow's delta. */
enum { PLACE_CHILDREN = 0, PLACE_BEFORE, PLACE_AFTER, PLACE_FIRST_CHILD, PLACE_REPLACE };

static void fragment_place(JSContext *ctx, lxb_dom_element_t *context, lxb_dom_node_t *anchor, int where,
                           const char *html)
{
    lxb_html_document_t *doc;
    lxb_dom_node_t *frag, *node, *next, *ref;
    static int in_parse;

    (void)ctx;
    doc = lxb_html_interface_document(lxb_dom_interface_node(context)->owner_document);
    DCHECK(!in_parse, "a fragment parse re-entered itself — Lexbor ran page code mid-parse, and a parser that "
                      "holds a continuation across the page cannot live in a suspending engine: give it the "
                      "flow treatment or keep the script out of the tree builder");
    in_parse = 1;
    frag = lxb_html_document_parse_fragment(doc, context, (const lxb_char_t *)html, strlen(html));
    in_parse = 0;
    if (!frag) return;
    /* The reference child is fixed BEFORE anything moves: inserting changes `anchor->next`. */
    ref = (where == PLACE_AFTER) ? anchor->next
        : (where == PLACE_FIRST_CHILD) ? anchor->first_child
        : anchor;
    for (node = frag->first_child; node; node = next) {
        next = node->next;
        lxb_dom_node_remove(node);   /* out of the fragment, which nothing else observes */
        switch (where) {
        case PLACE_CHILDREN:    dom_cow_append_child(anchor, node); break;
        case PLACE_BEFORE:
        case PLACE_REPLACE:     dom_cow_insert_before(anchor, node); break;
        case PLACE_AFTER:       if (ref) dom_cow_insert_before(ref, node);
                                else dom_cow_append_child(anchor->parent, node);
                                break;
        case PLACE_FIRST_CHILD: if (ref) dom_cow_insert_before(ref, node);
                                else dom_cow_append_child(anchor, node);
                                break;
        default: DFAIL("a fragment was placed with an unknown position"); break;
        }
    }
    lxb_dom_node_destroy(frag);
    if (where == PLACE_REPLACE) dom_cow_remove_child(anchor);
}

/* An HTML-context sink is TWO things and it must do both.
   It is a SINK, so the assigned value goes to the solver, which decides the breakout against the real parse
   context. And it MUTATES THE TREE — a page that builds its DOM this way and then queries it must find what it
   built, or every getElementById after it answers null and the engine reports a surface the page never had.
   Reporting the sink and dropping the markup was the second half missing.
   Both halves go through the per-flow chokepoints, so two forked arms each see their own subtree. A concolic
   value has no bytes to parse — the sink report IS the answer for it.
   magic 0 = innerHTML=, 1 = outerHTML=. */
static JSValue js_el_set_html(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic)
{
    lxb_dom_element_t *el = elem_of(this_val);
    lxb_dom_node_t *n, *node, *next;
    const char *html;

    if (!el) return JS_UNDEFINED;
    n = lxb_dom_interface_node(el);
    /* §4.9: an element with no parent cannot be replaced — there is nothing to replace it IN. */
    if (magic == 1 && (!n->parent || n->parent->type != LXB_DOM_NODE_TYPE_ELEMENT))
        return JS_ThrowDOMException(ctx, "NoModificationAllowedError",
                                    "outerHTML on an element with no element parent");
    solve_html_sink(ctx, val);
    if (concolic_is(val))
        return JS_UNDEFINED;   /* nothing concrete to parse; the sink is what this write means */

    DCHECK(JS_IsString(val), "an HTML sink reached the body unconverted — the IDL declaration is what converts "
                             "it, and running the page's toString from here is the drive-to-completion the flow "
                             "machinery exists to avoid");
    html = JS_ToCString(ctx, val);
    if (!html) return JS_EXCEPTION;
    if (magic == 0) {
        for (node = n->first_child; node; node = next) {
            next = node->next;
            dom_cow_remove_child(node);
        }
        fragment_place(ctx, el, n, PLACE_CHILDREN, html);
    } else {
        /* §4.9: the fragment is parsed in the PARENT's context, because that is where it is going to live. */
        fragment_place(ctx, lxb_dom_interface_element(n->parent), n, PLACE_REPLACE, html);
    }
    JS_FreeCString(ctx, html);
    return JS_UNDEFINED;
}

/* §4.9 insertAdjacentHTML / insertAdjacentElement / insertAdjacentText — the SAME four positions, which is why
   one body reads the position and three members differ only in what they place. insertAdjacentHTML is an
   HTML-context sink exactly like innerHTML, and it was absent: a bundle using it had its DOM unbuilt AND its
   XSS invisible, which is the pair this engine exists to report.
   magic 0 = HTML, 1 = Element, 2 = Text. */
static JSValue js_el_insert_adjacent(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                                     int magic)
{
    lxb_dom_element_t *el = elem_of(this_val);
    lxb_dom_node_t *n;
    const char *pos;
    int where;
    bool outside;

    if (!el || argc < 2) return JS_UNDEFINED;
    n = lxb_dom_interface_node(el);
    pos = JS_ToCString(ctx, argv[0]);   /* a real string by now: the declaration converted it */
    if (!pos) return JS_EXCEPTION;
    /* §4.9: the four positions, ASCII case-insensitively. Anything else is a SyntaxError, not a quiet no-op. */
    if (!strcasecmp(pos, "beforebegin"))    { where = PLACE_BEFORE;      outside = true;  }
    else if (!strcasecmp(pos, "afterbegin")) { where = PLACE_FIRST_CHILD; outside = false; }
    else if (!strcasecmp(pos, "beforeend"))  { where = PLACE_CHILDREN;    outside = false; }
    else if (!strcasecmp(pos, "afterend"))   { where = PLACE_AFTER;       outside = true;  }
    else {
        JS_FreeCString(ctx, pos);
        return JS_ThrowDOMException(ctx, "SyntaxError", "not one of the four adjacent positions");
    }
    JS_FreeCString(ctx, pos);
    if (outside && (!n->parent || n->parent->type != LXB_DOM_NODE_TYPE_ELEMENT))
        return JS_ThrowDOMException(ctx, "NoModificationAllowedError",
                                    "an adjacent position outside an element with no element parent");
    if (magic == 0) {
        const char *html;
        solve_html_sink(ctx, argv[1]);
        if (concolic_is(argv[1])) return JS_UNDEFINED;
        DCHECK(JS_IsString(argv[1]), "insertAdjacentHTML reached the body unconverted");
        html = JS_ToCString(ctx, argv[1]);
        if (!html) return JS_EXCEPTION;
        /* Parsed in the context it will LIVE in: the parent for the outside positions, this element for the
           inside ones. A `<td>` inserted beforeend of a `<tr>` survives; parsed against the wrong context it
           would be dropped by the tree builder and the page would find nothing it inserted. */
        fragment_place(ctx, outside ? lxb_dom_interface_element(n->parent) : el, n, where, html);
        JS_FreeCString(ctx, html);
        return JS_UNDEFINED;
    }
    {
        lxb_dom_node_t *added, *ref;
        if (magic == 1) {
            added = node_of(argv[1]);
            if (!added) return JS_ThrowTypeError(ctx, "insertAdjacentElement requires an Element");
        } else {
            const char *s;
            size_t slen = 0;
            lxb_dom_text_t *t;
            s = JS_ToCStringLen(ctx, &slen, argv[1]);
            if (!s) return JS_EXCEPTION;
            t = lxb_dom_document_create_text_node(n->owner_document, (const lxb_char_t *)s, slen);
            JS_FreeCString(ctx, s);
            if (!t) return JS_UNDEFINED;
            added = lxb_dom_interface_node(t);
        }
        ref = (where == PLACE_AFTER) ? n->next : (where == PLACE_FIRST_CHILD ? n->first_child : n);
        switch (where) {
        case PLACE_BEFORE:      dom_cow_insert_before(n, added); break;
        case PLACE_CHILDREN:    dom_cow_append_child(n, added); break;
        case PLACE_AFTER:       if (ref) dom_cow_insert_before(ref, added);
                                else dom_cow_append_child(n->parent, added);
                                break;
        case PLACE_FIRST_CHILD: if (ref) dom_cow_insert_before(ref, added);
                                else dom_cow_append_child(n, added);
                                break;
        default: DFAIL("insertAdjacent ran with an unknown position"); break;
        }
        /* §4.9: insertAdjacentElement returns the inserted node; insertAdjacentText returns undefined. */
        return magic == 1 ? JS_DupValue(ctx, argv[1]) : JS_UNDEFINED;
    }
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

/* §4.9 matches / closest — the two questions a router asks. `matches` is the single-node match; `closest`
   walks INCLUSIVE ancestors and answers the first that matches, which is how a delegated click handler finds
   the row it belongs to. Both are the selector engine's, not a second reading of a selector.
   magic 0 = matches, 1 = closest. */
static JSValue js_el_matches(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    lxb_dom_element_t *el = elem_of(this_val);
    lxb_dom_node_t *n;
    const char *sel;
    int m = 0;

    if (!el || argc < 1) return magic ? JS_NULL : JS_FALSE;
    sel = JS_ToCString(ctx, argv[0]);   /* a real string by now: the declaration converted it */
    if (!sel) return JS_EXCEPTION;
    for (n = lxb_dom_interface_node(el); n; n = n->parent) {
        if (n->type != LXB_DOM_NODE_TYPE_ELEMENT) break;
        m = document_sel_match(n, sel);
        if (m != 0) break;                 /* matched, or the selector is invalid */
        if (!magic) break;                 /* `matches` asks about this element alone */
    }
    JS_FreeCString(ctx, sel);
    /* §4.9: an unparseable selector is a SyntaxError, never a quiet "no". */
    if (m < 0) return JS_ThrowDOMException(ctx, "SyntaxError", "not a valid selector");
    if (magic) return m > 0 ? node_wrap(ctx, n) : JS_NULL;
    return JS_NewBool(ctx, m > 0);
}

/* §4.2.6 children / firstElementChild / lastElementChild / childElementCount — the ELEMENT-only walk beside
   Node's childNodes, which a page uses precisely to skip the whitespace Text nodes a parser leaves behind.
   `children` is a LIVE HTMLCollection with a NAMED getter, which is how a great deal of older code reaches its
   own markup (`form.children.email`); the rest are plain reads of the tree. */
static JSValue js_el_children(JSContext *ctx, JSValueConst this_val, int magic)
{
    lxb_dom_element_t *el = elem_of(this_val);
    lxb_dom_node_t *c, *first = NULL, *last = NULL;
    uint32_t n = 0;
    JSValue arr = JS_UNDEFINED;

    if (!el) return magic == 0 ? JS_NewArray(ctx) : (magic == 3 ? JS_NewInt32(ctx, 0) : JS_NULL);
    if (magic == 0) return collections_children(ctx, this_val);
    for (c = lxb_dom_interface_node(el)->first_child; c; c = c->next) {
        if (c->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        if (!first) first = c;
        last = c;
        n++;
    }
    switch (magic) {
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

/* §4.2.3's INSERTION and REMOVING STEPS, over the whole SUBTREE — inserting a subtree connects every element
   in it, and a page building its UI off-tree and appending the root once is the ordinary case, not a corner.
   Walking only the inserted node meant a custom element inside a built fragment was never upgraded and its
   lifecycle code never ran, which is precisely the code this engine exists to reach.
   Only a CONNECTED node has these steps run for it: §4.13.3 upgrades on entering a document, and a subtree
   moved between two detached parents has entered nothing. */
static void element_tree_changed(JSContext *ctx, lxb_dom_node_t *root, int inserted)
{
    lxb_dom_node_t *n = root;

    if (!root || !node_is_connected(root)) return;
    for (;;) {
        if (n->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            lxb_dom_element_t *el = lxb_dom_interface_element(n);
            if (inserted) {
                element_prepare_script(ctx, el);   /* HTML 4.12.1: an inserted <script> is PREPARED */
                /* §4.13.3: an element that ENTERS a document is upgraded if its name is defined — the other
                   half of "learned by execution", beside the <script> preparation right above it. */
                custom_elements_try_upgrade(ctx, el);
            } else {
                custom_elements_disconnected(ctx, el);
            }
        }
        if (n->first_child) { n = n->first_child; continue; }
        while (n && !n->next) n = (n == root) ? NULL : n->parent;
        n = n ? n->next : NULL;
        if (!n) break;
    }
}

/* §4.9's attribute change steps, fired by the mutation chokepoint for the same reason the tree steps are:
   `setAttribute`, a reflected IDL attribute, a boolean reflection unsetting itself and innerHTML's parse all
   reach the tree through one function, and a per-caller notification would miss whichever one was added last. */
static void element_attr_changed(JSContext *ctx, lxb_dom_element_t *el, const char *name,
                                 const char *val, size_t val_len)
{
    custom_elements_attribute_changed(ctx, el, name, val, val_len);
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
    {
        /* §4.9: webkitMatchesSelector is `matches` under its historical name — the IDL declares it as the same
           operation, so it IS the same declaration and not a forwarding wrapper. */
        int m = idl_method_id(ctx, IDL_1STR, 1, js_el_matches, 0);
        idl_install_method(ctx, proto, "matches", 1, m);
        idl_install_method(ctx, proto, "webkitMatchesSelector", 1, m);
        idl_install_method(ctx, proto, "closest", 1,
                           idl_method_id(ctx, IDL_1STR, 1, js_el_matches, 1));
    }
    idl_indexed_init(ctx);      /* the exotic class every indexed interface is built on */
    collections_init(ctx);      /* NodeList and HTMLCollection, which childNodes and children are */
    dom_token_list_init(ctx);   /* its prototype must exist before classList names it */
    dom_token_list_install_element(ctx, proto);   /* §4.9's [SameObject] classList */
    node_install_child_mixin(ctx, proto);    /* remove / before / after / replaceWith */
    node_install_parent_mixin(ctx, proto);   /* append / prepend / replaceChildren */

    /* `[CEReactions] attribute [LegacyNullToEmptyString] DOMString innerHTML` — the extended attribute is part
       of the TYPE, so `el.innerHTML = null` empties the element instead of parsing the markup `null`. */
    idl_install_accessor(ctx, proto, "innerHTML", js_el_get_html, 0,
                         idl_setter_id(ctx, IDL_DOMSTRING, true, js_el_set_html, 0));
    idl_install_accessor(ctx, proto, "outerHTML", js_el_get_html, 1,
                         idl_setter_id(ctx, IDL_DOMSTRING, true, js_el_set_html, 1));
    {
        /* §4.9's three adjacent members. The HTML one takes two DOMStrings; the other two take a position and
           a value the IDL leaves alone (a Node, or a DOMString this stringifies into a Text node). */
        static const IdlArgType ADJ_ANY[2] = { IDL_DOMSTRING, IDL_ANY };
        idl_install_method(ctx, proto, "insertAdjacentHTML", 2,
                           idl_method_id(ctx, IDL_2STR, 2, js_el_insert_adjacent, 0));
        idl_install_method(ctx, proto, "insertAdjacentElement", 2,
                           idl_method_id(ctx, ADJ_ANY, 2, js_el_insert_adjacent, 1));
        idl_install_method(ctx, proto, "insertAdjacentText", 2,
                           idl_method_id(ctx, IDL_2STR, 2, js_el_insert_adjacent, 2));
    }

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
    node_set_tree_hook(element_tree_changed);
    dom_cow_set_attr_hook(element_attr_changed);
    custom_elements_init(ctx);
    cssom_init(ctx);          /* CSSStyleDeclaration.prototype, which HTMLElement's `style` attribute names */
    html_element_init(ctx);   /* the HTML half, which builds HTMLElement and the per-tag interfaces on this */
}

JSValueConst element_proto(void) { DCHECK(JS_IsObject(g_element_proto), "Element.prototype was asked for before element_init built it"); return g_element_proto; }

void element_free(JSContext *ctx)
{
    html_element_free(ctx);
    cssom_free(ctx);
    custom_elements_free(ctx);
    dom_token_list_free(ctx);
    collections_free(ctx);
    idl_indexed_free(ctx);
    JS_FreeValue(ctx, g_element_proto);
    g_element_proto = JS_UNDEFINED;
    g_reflect_n = 0;
    node_free(ctx);
}
