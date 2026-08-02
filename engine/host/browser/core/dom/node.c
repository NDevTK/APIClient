/* THE NODE INTERFACE — DOM §4.4, and the CharacterData nodes on top of it (§4.10).
 *
 * WHY THIS EXISTS SEPARATELY FROM element.c. The wrapper table used to be keyed on lxb_dom_element_t, so an
 * ELEMENT was the only thing this engine could hand a page. Everything else in a real tree — Text, Comment —
 * had no wrapper at all, and the consequences were four separate lines in the WPT gap list that were all this
 * one hole: `document.createTextNode` was "not a function"; appendChild and removeChild asserted on "something
 * that is not an element wrapper"; and `Text`/`Comment` were undefined globals. A tree API whose only node kind
 * is Element cannot represent the document it just parsed.
 *
 * IDENTITY IS THE INVARIANT, and it is the reason this is a table rather than a fresh object per call. A page
 * compares nodes constantly (`n === el.firstChild`, a Set of visited nodes, a WeakMap keyed by node), and a
 * fresh wrapper per lookup makes every one of those silently false — the page then re-walks, re-binds and
 * re-inserts, and the engine reports a surface built out of that confusion instead of the page's.
 *
 * ONE CLASS, PROTOTYPE BY TYPE. Every wrapper is the same JS class (so one opaque, one table, one identity
 * rule); which members it carries is decided by the node's Lexbor type. That keeps `node_of` total — any
 * wrapper answers with its node — while an Element-only member still asserts it is on an Element.
 *
 * READS are pure Lexbor and run no page code, so they are ordinary C. WRITES go through the solver's
 * chokepoints (dom_cow_*) because a DOM write is per-flow TIME-TRAVEL state: two forked arms mutate the same
 * tree differently and each reads back its own. */
#include <stdlib.h>
#include <string.h>

#include <lexbor/dom/dom.h>

#include "check.h"
#include "quickjs.h"
#include "solver/dom_cow.h"
#include "core/dom/node.h"
#include "core/events/event_target.h"

static JSClassID g_node_class;

typedef struct { lxb_dom_node_t *n; JSValue obj; } NodeEntry;
static NodeEntry *g_wraps;
static int        g_wrap_n, g_wrap_cap;

JSClassID node_class_id(void) { return g_node_class; }

lxb_dom_node_t *node_of(JSValueConst v)
{
    return JS_GetOpaque(v, g_node_class);
}

/* §4.4 nodeType — the numeric constants a page switches on. */
static JSValue js_node_get_type(JSContext *ctx, JSValueConst this_val)
{
    lxb_dom_node_t *n = node_of(this_val);
    if (!n) return JS_UNDEFINED;
    return JS_NewInt32(ctx, (int)n->type);
}

/* §4.4 nodeName: an element's is its qualified name uppercased by the HTML serialiser Lexbor already applies;
   a text node's is "#text" and a comment's "#comment", which the spec fixes rather than derives. */
static JSValue js_node_get_name(JSContext *ctx, JSValueConst this_val)
{
    lxb_dom_node_t *n = node_of(this_val);
    size_t len = 0;
    const lxb_char_t *s;

    if (!n) return JS_UNDEFINED;
    if (n->type == LXB_DOM_NODE_TYPE_TEXT)    return JS_NewString(ctx, "#text");
    if (n->type == LXB_DOM_NODE_TYPE_COMMENT) return JS_NewString(ctx, "#comment");
    s = lxb_dom_node_name(n, &len);
    return s ? JS_NewStringLen(ctx, (const char *)s, len) : JS_NewString(ctx, "");
}

/* §4.4 the tree accessors. Pure pointer reads off the tree the running flow sees — which is the flow's own
   tree, because every mutation went through the COW delta. */
static JSValue js_node_tree(JSContext *ctx, JSValueConst this_val, int magic)
{
    lxb_dom_node_t *n = node_of(this_val);
    if (!n) return JS_NULL;
    switch (magic) {
    case 0: return node_wrap(ctx, n->parent);
    case 1: return node_wrap(ctx, n->first_child);
    case 2: return node_wrap(ctx, n->last_child);
    case 3: return node_wrap(ctx, n->next);
    case 4: return node_wrap(ctx, n->prev);
    default: DFAIL("node tree accessor with an unknown magic"); return JS_NULL;
    }
}

/* §4.4 childNodes. A STATIC array, not the spec's live NodeList: a page that inserts a child and re-reads
   `.length` will not see the change. Named here rather than papered over — it is the same shape
   querySelectorAll and getElementsByTagName already return, and the live collection is its own component. */
static JSValue js_node_child_nodes(JSContext *ctx, JSValueConst this_val)
{
    lxb_dom_node_t *n = node_of(this_val), *c;
    JSValue arr;
    uint32_t i = 0;

    if (!n) return JS_NewArray(ctx);
    arr = JS_NewArray(ctx);
    for (c = n->first_child; c; c = c->next)
        JS_SetPropertyUint32(ctx, arr, i++, node_wrap(ctx, c));
    return arr;
}

/* §4.2.3 appendChild / removeChild — the per-flow chokepoints, and they return the node the spec returns
   (pages chain on it). Any node kind, which is the whole point of this file. */
static JSValue js_node_append_child(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
static JSValue js_node_remove_child(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);

/* A newly inserted <script> is PREPARED (HTML 4.12.1) by the element component, which owns that rule; the base
   asks for it through this hook so node.c does not have to know what a script is. */
static void (*g_inserted_hook)(JSContext *ctx, lxb_dom_node_t *n);
void node_set_inserted_hook(void (*fn)(JSContext *ctx, lxb_dom_node_t *n)) { g_inserted_hook = fn; }

static JSValue js_node_append_child(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    lxb_dom_node_t *n = node_of(this_val), *child;

    if (!n || argc < 1) return JS_UNDEFINED;
    child = node_of(argv[0]);
    if (!child)
        return JS_ThrowTypeError(ctx, "appendChild requires a Node");
    dom_cow_append_child(n, child);
    if (g_inserted_hook) g_inserted_hook(ctx, child);
    return JS_DupValue(ctx, argv[0]);
}

static JSValue js_node_remove_child(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    lxb_dom_node_t *n = node_of(this_val), *child;

    if (!n || argc < 1) return JS_UNDEFINED;
    child = node_of(argv[0]);
    if (!child)
        return JS_ThrowTypeError(ctx, "removeChild requires a Node");
    if (child->parent != n)
        return JS_ThrowDOMException(ctx, "NotFoundError", "the node to remove is not a child of this node");
    dom_cow_remove_child(child);
    return JS_DupValue(ctx, argv[0]);
}

/* §4.10 CharacterData.data / .length — the text a Text or Comment node holds. */
static JSValue js_cd_get_data(JSContext *ctx, JSValueConst this_val)
{
    lxb_dom_node_t *n = node_of(this_val);
    lxb_dom_character_data_t *cd;

    if (!n) return JS_UNDEFINED;
    DCHECK(n->type == LXB_DOM_NODE_TYPE_TEXT || n->type == LXB_DOM_NODE_TYPE_COMMENT,
           "CharacterData.data was read on a node that holds no character data");
    cd = lxb_dom_interface_character_data(n);
    return JS_NewStringLen(ctx, (const char *)cd->data.data, cd->data.length);
}

static JSValue js_cd_set_data(JSContext *ctx, JSValueConst this_val, JSValueConst val)
{
    lxb_dom_node_t *n = node_of(this_val);
    const char *s;

    if (!n) return JS_UNDEFINED;
    {
        size_t len = 0;
        s = JS_ToCStringLen(ctx, &len, val);
        if (!s) return JS_EXCEPTION;
        dom_cow_set_text(n, s, len);
        JS_FreeCString(ctx, s);
    }
    return JS_UNDEFINED;
}

static JSValue js_cd_get_length(JSContext *ctx, JSValueConst this_val)
{
    lxb_dom_node_t *n = node_of(this_val);
    if (!n) return JS_NewInt32(ctx, 0);
    return JS_NewInt32(ctx, (int)lxb_dom_interface_character_data(n)->data.length);
}

static const JSCFunctionListEntry js_node_base[] = {
    JS_CFUNC_DEF("appendChild", 1, js_node_append_child),
    JS_CFUNC_DEF("removeChild", 1, js_node_remove_child),
    JS_CGETSET_DEF("nodeType", js_node_get_type, NULL),
    JS_CGETSET_DEF("nodeName", js_node_get_name, NULL),
    JS_CGETSET_DEF("childNodes", js_node_child_nodes, NULL),
    JS_CGETSET_MAGIC_DEF("parentNode", js_node_tree, NULL, 0),
    JS_CGETSET_MAGIC_DEF("firstChild", js_node_tree, NULL, 1),
    JS_CGETSET_MAGIC_DEF("lastChild", js_node_tree, NULL, 2),
    JS_CGETSET_MAGIC_DEF("nextSibling", js_node_tree, NULL, 3),
    JS_CGETSET_MAGIC_DEF("previousSibling", js_node_tree, NULL, 4),
};

static const JSCFunctionListEntry js_chardata_proto[] = {
    JS_CGETSET_DEF("data", js_cd_get_data, js_cd_set_data),
    JS_CGETSET_DEF("length", js_cd_get_length, NULL),
    JS_CGETSET_DEF("nodeValue", js_cd_get_data, js_cd_set_data),
};

void node_install_base(JSContext *ctx, JSValueConst obj)
{
    JS_SetPropertyFunctionList(ctx, (JSValue)obj, js_node_base,
                               (int)(sizeof(js_node_base) / sizeof(js_node_base[0])));
    /* §4.4: `interface Node : EventTarget`. Every node is one, and only the global was — so
       `el.addEventListener(...)` was "not a function" on every element a page wired up, which is where
       testharness.js stopped on eight documents (`getElementById("rerun").addEventListener`). Installed on the
       base because it is a base member, not an Element one. */
    event_target_install(ctx, obj);
}

/* The ELEMENT members are element.c's to install; the base and the character-data members are this file's. The
   component that owns a kind registers its installer here so node_wrap stays the ONE place a wrapper is built —
   two places building wrappers is two identity tables, which is no identity at all. */
static void (*g_element_installer)(JSContext *ctx, JSValueConst obj);
void node_set_element_installer(void (*fn)(JSContext *ctx, JSValueConst obj)) { g_element_installer = fn; }

JSValue node_wrap(JSContext *ctx, lxb_dom_node_t *n)
{
    JSValue obj;
    int i;

    if (!n)
        return JS_NULL;
    for (i = 0; i < g_wrap_n; i++)
        if (g_wraps[i].n == n)
            return JS_DupValue(ctx, g_wraps[i].obj);

    obj = JS_NewObjectClass(ctx, g_node_class);
    if (JS_IsException(obj))
        return obj;
    JS_SetOpaque(obj, n);
    node_install_base(ctx, obj);
    if (n->type == LXB_DOM_NODE_TYPE_ELEMENT) {
        DCHECK(g_element_installer != NULL, "an Element node was wrapped before element.c registered its members");
        if (g_element_installer) g_element_installer(ctx, obj);
    } else if (n->type == LXB_DOM_NODE_TYPE_TEXT || n->type == LXB_DOM_NODE_TYPE_COMMENT) {
        JS_SetPropertyFunctionList(ctx, obj, js_chardata_proto,
                                   (int)(sizeof(js_chardata_proto) / sizeof(js_chardata_proto[0])));
    }

    if (g_wrap_n == g_wrap_cap) {
        int c = g_wrap_cap ? g_wrap_cap * 2 : 16;
        NodeEntry *a = realloc(g_wraps, sizeof(*a) * (size_t)c);
        CHECK(a != NULL, "the node wrapper table allocation failed: a dropped wrapper breaks node identity, and "
                         "every `n === other` the page makes after it is silently false");
        g_wraps = a; g_wrap_cap = c;
    }
    g_wraps[g_wrap_n].n = n;
    g_wraps[g_wrap_n].obj = JS_DupValue(ctx, obj);
    g_wrap_n++;
    return obj;
}

static void node_finalizer(JSRuntime *rt, JSValue val) { (void)rt; (void)val; }

void node_init(JSContext *ctx)
{
    JSClassDef def = { "Node", .finalizer = node_finalizer };
    JS_NewClassID(JS_GetRuntime(ctx), &g_node_class);
    JS_NewClass(JS_GetRuntime(ctx), g_node_class, &def);
}

void node_free(JSContext *ctx)
{
    int i;
    for (i = 0; i < g_wrap_n; i++)
        JS_FreeValue(ctx, g_wraps[i].obj);
    free(g_wraps);
    g_wraps = NULL; g_wrap_n = g_wrap_cap = 0;
}
