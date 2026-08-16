/* The `script` element's parse state and HTML §4.12.1's preparation — see html_script.h for why one boolean
   with three call sites is a component. */
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include <lexbor/dom/dom.h>
#include <lexbor/html/html.h>
#include <lexbor/ns/ns.h>

#include "check.h"
#include "quickjs.h"
#include "solver/dom_cow.h"
#include "solver/endpoint.h"
#include "solver/engine.h"
#include "core/dom/node.h"
#include "core/dom/document.h"   /* which DOCUMENT this program belongs to: the realm it is compiled in */
#include "core/html/html_script.h"

/* §4.12.1's `already started`, on the element's wrapper under a Symbol this file minted and never published —
   the store DOM §4.9's custom element state uses, for the two reasons html_script.h gives. */
static JSValue g_started_key = JS_UNDEFINED;
static JSAtom  g_atom_started = JS_ATOM_NULL;

/* CONFIGURABLE AND WRITABLE for the reason custom_elements.c's slots are: the flag is written more than once
   over one element's life — the parse marks it, and §4.12.1's cloning steps write the copy's from the
   original's — and a slot defined with no flags makes the second write a silent no-op. */
#define SCRIPT_SLOT_FLAGS (JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE)

void html_script_init(JSContext *ctx)
{
    DCHECK(g_atom_started == JS_ATOM_NULL, "html_script_init ran twice in one runtime — the slot key is a "
                                           "Symbol, and a second one would leave every element already marked "
                                           "under the first key answering false under the second");
    g_started_key = JS_NewSymbol(ctx, "scriptAlreadyStarted", false);
    CHECK(!JS_IsException(g_started_key), "the script already-started slot key allocation failed");
    g_atom_started = JS_ValueToAtom(ctx, g_started_key);
    CHECK(g_atom_started != JS_ATOM_NULL, "the script already-started slot key could not be interned");
}

void html_script_free(JSContext *ctx)
{
    JS_FreeAtom(ctx, g_atom_started);
    g_atom_started = JS_ATOM_NULL;
    JS_FreeValue(ctx, g_started_key);
    g_started_key = JS_UNDEFINED;
}

/* IS THIS NODE A `script` ELEMENT? The INTERNED TAG ID and the pair of namespaces a `script` can be in, which
   is the same composite test §8.6.4 step 3 makes a few hundred lines away in element.c — HTML's `script` and
   SVG's are both script elements, and lexbor's own `lxb_html_tree_node_is` answers only for the first because
   it hardcodes the HTML namespace. It replaces a memcmp over the QUALIFIED name, which is the same set by
   accident (a prefixed `foo:script` does not match six bytes) and says nothing about why. */
static bool script_is(const lxb_dom_node_t *n)
{
    return n != NULL && n->type == LXB_DOM_NODE_TYPE_ELEMENT &&
           n->local_name == LXB_TAG_SCRIPT && (n->ns == LXB_NS_HTML || n->ns == LXB_NS_SVG);
}

/* §4.12.1's `already started` for an element. ABSENT IS FALSE — the standard's own initial value — so this
   reads through node_wrap_peek and never mints a wrapper: an element nothing has marked is an element nothing
   has written, and allocating one to learn a default would put a wrapper on every `<script>` a page inserts. */
static bool script_already_started(JSContext *ctx, const lxb_dom_node_t *n)
{
    JSValueConst wrap;
    JSValue v;
    int r;

    DCHECK(g_atom_started != JS_ATOM_NULL,
           "a script's `already started` was asked for before html_script_init minted its slot key");
    wrap = node_wrap_peek(n);
    if (!JS_IsObject(wrap)) return false;
    r = JS_GetOwnSlot(ctx, &v, wrap, g_atom_started);
    if (r <= 0) return false;
    DCHECK(JS_IsBool(v), "a script's `already started` slot holds something that is not a boolean — the slot is "
                         "written by html_script.c and by nothing else");
    r = JS_ToBool(ctx, v);
    JS_FreeValue(ctx, v);
    return r != 0;
}

/* Write it. This one DOES mint the wrapper, because there is nowhere else for the fact to live — and it is
   reached only for an element some parse or clone actually marked, so the allocation is one per inert script
   rather than one per script. */
static void script_set_already_started(JSContext *ctx, lxb_dom_node_t *n)
{
    JSValue wrap;

    DCHECK(g_atom_started != JS_ATOM_NULL,
           "a script's `already started` was written before html_script_init minted its slot key");
    DCHECK(script_is(n), "`already started` was written onto a node that is not an HTML `script` element");
    wrap = node_wrap(ctx, n);
    CHECK(JS_IsObject(wrap), "a script element could not be wrapped to carry its `already started` — an "
                             "unmarked script is one §4.12.1 step 1 lets run, so failing quietly here would "
                             "execute markup the fragment parse is required to keep inert");
    JS_DefinePropertyValue(ctx, wrap, g_atom_started, JS_TRUE, SCRIPT_SLOT_FLAGS);
    JS_FreeValue(ctx, wrap);
}

/* A `<template>`'s CONTENT FRAGMENT, or NULL — a tree reached other than through child links, and the reason
   this walk is not the three-line one beside it. */
static lxb_dom_node_t *template_content(lxb_dom_node_t *n)
{
    lxb_html_template_element_t *t;

    if (n->type != LXB_DOM_NODE_TYPE_ELEMENT || !lxb_html_tree_node_is(n, LXB_TAG_TEMPLATE)) return NULL;
    t = lxb_html_interface_template(n);
    return t->content ? &t->content->node : NULL;
}

void html_script_parsed_inert(JSContext *ctx, lxb_dom_node_t *root)
{
    lxb_dom_node_t *n = root, *content;

    if (!root) return;
    /* AN ITERATIVE DESCENT, like dom_attr_normalize_parsed's over the same tree at the same moment: this is a
       parse product, so its depth is the markup's, and a recursive walk would put the page's nesting on the C
       stack — which §C-stack is the whole reason nothing in this engine does.
       IT ENTERS `<template>` CONTENTS, which the walk beside it does not have to. The parser puts a template's
       markup in its CONTENT FRAGMENT, and a `<script>` in there was created by THIS parse under the same Inert
       mode, so it is already started too — and it is reachable: `t.content.cloneNode(true)` copies it out, and
       §4.12.1's cloning steps carry the flag with it, so an unmarked one would run from the clone. lexbor
       leaves the fragment's `parent` NULL and points its `host` back at the element, which is what the ascent
       climbs; a template can hold BOTH lists (only the parser and `t.content` reach the fragment, while
       `t.appendChild(x)` reaches the element), so coming back visits the ordinary children next. */
    for (;;) {
        if (script_is(n)) script_set_already_started(ctx, n);
        content = template_content(n);
        if (content && content->first_child) { n = content->first_child; continue; }
    children:
        if (n->first_child) { n = n->first_child; continue; }
        for (;;) {
            if (n == root) return;
            if (n->next) { n = n->next; break; }
            if (n->type == LXB_DOM_NODE_TYPE_DOCUMENT_FRAGMENT &&
                lxb_dom_interface_document_fragment(n)->host != NULL) {
                n = lxb_dom_interface_node(lxb_dom_interface_document_fragment(n)->host);
                goto children;
            }
            n = n->parent;
            DCHECK(n != NULL,
                   "the Inert marking walked off the top of the tree it was given — every node it reaches is "
                   "either under `root` or in a `<template>` content fragment whose host is, so a null parent "
                   "means the parse handed back a node that is in neither");
        }
    }
}

void html_script_cloned(JSContext *ctx, lxb_dom_node_t *src, lxb_dom_node_t *copy)
{
    if (!script_is(src)) return;
    DCHECK(script_is(copy),
           "DOM §4.4 clone a node produced a copy of a `script` element that is not one — the cloning steps "
           "HTML defines for `script` are stated over a copy of the same element, and a pair that disagrees "
           "means step 2's `clone a single node` built the wrong interface");
    /* "Set copy's already started to node's already started." FALSE is the copy's initial value and there is
       no slot to clear — a fresh element has never been written — so only the true case has anything to do. */
    if (script_already_started(ctx, src)) script_set_already_started(ctx, copy);
}

/* HTML §4.12.1 "prepare the script element", the INSERTION half — DOM §4.2.3's insertion steps are what reach
 * it. A page loads code conditionally in three ways and this is the second:
 * `s = createElement("script"); s.src = u; body.appendChild(s)`. Before this existed the injection was a SILENT
 * no-op — the element went into the tree and the code it named was never fetched, never run, never even
 * reported, so every endpoint and sink behind an A/B flag or a feature gate was missing with nothing to say so.
 * The loaded code is more PROGRAM OF THE INJECTING FLOW: it joins that flow's script sequence, so it runs under
 * the delta, the pins and the position in the BFS of the world that injected it, and a sibling that never took
 * the branch never sees it. */
void html_script_prepare(JSContext *ctx, lxb_dom_element_t *el)
{
    lxb_dom_node_t *n = lxb_dom_interface_node(el);
    size_t n_len = 0;
    const lxb_char_t *src;
    JSValue t;

    if (!script_is(n)) return;
    /* STEP 1: "If el's already started is true, then return." This is the whole of what makes §13.4's fragment
       parse inert — the parsed script is in the tree, is queryable, serialises back out, and does not run. */
    if (script_already_started(ctx, n)) return;
    /* An UNKNOWN src is a URL this engine cannot fetch, but it is still a request the page makes — recorded so
       it reaches the @H surface as the shape it is, rather than disappearing. */
    t = dom_cow_attr_taint(el, "src");
    if (!JS_IsUndefined(t)) {
        endpoint_record(ctx, "GET", t, NULL, 0);
        return;
    }
    src = lxb_dom_element_get_attribute(el, (const lxb_char_t *)"src", 3, &n_len);
    if (src && n_len) {
        char *u = malloc(n_len + 1);
        CHECK(u, "html_script: OOM copying an injected script's URL");
        memcpy(u, src, n_len); u[n_len] = 0;
        engine_pending_script_url(ctx, u);
        free(u);
        return;
    }
    /* No src: the element's own text IS the program, and it runs on insertion. */
    {
        lxb_char_t *txt = lxb_dom_node_text_content(n, &n_len);
        if (txt) {
            /* IN THE DOCUMENT WHOSE TREE IT WAS INSERTED INTO — "prepare the script" runs it with the
               element's node document's settings object, which is the realm this chokepoint was entered
               with. A program is a program OF a document (solver/flow.h), so it names one. */
            if (n_len) engine_queue_script(document_doc(ctx), (const char *)txt);
            lxb_dom_document_destroy_text(n->owner_document, txt);
        }
    }
}
