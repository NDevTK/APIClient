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
 * AND THE PROTOTYPE IS A REAL PROTOTYPE. The members used to be COPIED onto every wrapper — node_wrap ran
 * JS_SetPropertyFunctionList, and element.c registered an INSTALLER callback that ran again per element. That is
 * not what an interface is, and three things followed from it. `a.getAttribute === b.getAttribute` was FALSE for
 * two elements of one document, so a page that lifts a method off one node and applies it to another (which the
 * spec guarantees) was comparing and caching against a different function every time. There was no
 * `Element.prototype`, so `Element.prototype.contains.call(x)` and every feature test spelled that way found
 * nothing. And every member cost one closure per node.
 *
 * So the interfaces are prototype objects and a wrapper is one JS_NewObjectProtoClass with nothing installed on
 * it. Which prototype a node gets is its Lexbor TYPE mapped through the DOM's own interface hierarchy: Element
 * for an element, CharacterData for Text and Comment, Node for everything else. A component that owns a derived
 * interface REGISTERS its prototype here (element.c owns Element) so that node_wrap stays the ONE place a
 * wrapper is built — two builders is two identity tables, which is no identity at all.
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
#include "solver/concolic.h"
#include "solver/attr_shadow.h"
#include "core/dom/node.h"
#include "core/idl_args.h"
#include "core/events/event_target.h"

static JSClassID g_node_class;

/* THE INTERFACE PER NODE TYPE. Indexed by lxb_dom_node_type_t, filled with Node.prototype at init so that a
   node kind no derived component claims is honestly a Node rather than a bare object. */
static JSValue g_protos[LXB_DOM_NODE_TYPE_LAST_ENTRY];
static JSValue g_node_proto;
static int     g_protos_ready;

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

static bool node_is_chardata(const lxb_dom_node_t *n)
{
    return n->type == LXB_DOM_NODE_TYPE_TEXT || n->type == LXB_DOM_NODE_TYPE_COMMENT;
}

/* §4.4 Node.nodeValue and §4.10 CharacterData.data — THE SAME TEXT THROUGH TWO MEMBERS WITH DIFFERENT RULES,
   which is why one body takes a magic rather than one member aliasing the other (magic 0 = data, 1 = nodeValue).
   They were aliased and both lived on the character-data members, which got the hierarchy and the types wrong in
   three ways at once. nodeValue is a member of NODE, so `element.nodeValue` must answer null and not be absent.
   nodeValue is `DOMString?`, so `n.nodeValue = null` empties the text — writing the four characters `null` into
   the page's DOM, which is what a ToString did, is the setter skipping its own first step. And data is
   `[LegacyNullToEmptyString] DOMString` on CharacterData, so it is brand-checked rather than silently answering
   for an element. */
static JSValue js_cd_get_data(JSContext *ctx, JSValueConst this_val, int magic)
{
    lxb_dom_node_t *n = node_of(this_val);
    lxb_dom_character_data_t *cd;

    if (!n) return JS_UNDEFINED;
    if (!node_is_chardata(n)) {
        if (magic == 1)
            return JS_NULL;    /* §4.4: nodeValue of anything that is not character data is null */
        return JS_ThrowTypeError(ctx, "CharacterData.data read on a node that holds no character data");
    }
    cd = lxb_dom_interface_character_data(n);
    return JS_NewStringLen(ctx, (const char *)cd->data.data, cd->data.length);
}

/* The value arrives CONVERTED — a real string, JS_NULL for the nullable member, or a concolic that crossed as
   itself. Nothing here runs the page's code, which is the claim the declaration in node_init makes. */
static JSValue js_cd_set_data(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic)
{
    lxb_dom_node_t *n = node_of(this_val);
    const char *s;
    size_t len;

    if (!n) return JS_UNDEFINED;
    if (!node_is_chardata(n)) {
        if (magic == 1)
            return JS_UNDEFINED;   /* §4.4 nodeValue setter: "otherwise: do nothing" */
        return JS_ThrowTypeError(ctx, "CharacterData.data set on a node that holds no character data");
    }
    /* §4.4 the nodeValue setter's own first step; data reaches this through [LegacyNullToEmptyString]. */
    if (JS_IsNull(val)) {
        dom_cow_set_text(n, "", 0);
        return JS_UNDEFINED;
    }
    /* Unknown external input has no bytes: its SHAPE is what the node carries, the same answer textContent
       gives, so a source parked in the tree as text still displays as the source it came from. */
    if (concolic_is(val)) {
        s = concolic_shape_c(val);
        dom_cow_set_text(n, s ? s : "", s ? strlen(s) : 0);
        return JS_UNDEFINED;
    }
    DCHECK(JS_IsString(val), "a CharacterData write reached the body unconverted — the IDL declaration is what "
                             "converts it, and running the page's toString from here is the drive-to-completion "
                             "the flow machinery exists to avoid");
    s = JS_ToCStringLen(ctx, &len, val);
    if (!s) return JS_EXCEPTION;
    dom_cow_set_text(n, s, len);
    JS_FreeCString(ctx, s);
    return JS_UNDEFINED;
}

static JSValue js_cd_get_length(JSContext *ctx, JSValueConst this_val)
{
    lxb_dom_node_t *n = node_of(this_val);
    if (!n) return JS_NewInt32(ctx, 0);
    if (!node_is_chardata(n))
        return JS_ThrowTypeError(ctx, "CharacterData.length read on a node that holds no character data");
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

/* §4.4 Node.textContent — a NODE member, which is where it now lives. It was an Element one, so
   `textNode.textContent` was undefined and `document.textContent` answered a string the spec says is null; and
   because Element was the only kind that had it, the same body had to be both the element algorithm and the
   character-data one. Here each branch is the spec's own case, and the shared "string replace all" is written
   once.
   NOT A MARKUP SINK — the setter creates ONE Text node and the getter concatenates descendant text — which is
   exactly why a page told to stop using innerHTML uses it, and why an engine lacking it saw those pages build
   nothing. The taint travels the way an attribute's does: the assigned concolic is recorded on the element's
   property slot, so a source parked in the DOM as text and later read back into a real sink is still solved. */
static bool node_has_children_as_text(const lxb_dom_node_t *n)
{
    return n->type == LXB_DOM_NODE_TYPE_ELEMENT || n->type == LXB_DOM_NODE_TYPE_DOCUMENT_FRAGMENT;
}

static JSValue js_node_get_text_content(JSContext *ctx, JSValueConst this_val, int magic)
{
    lxb_dom_node_t *n = node_of(this_val);
    lxb_char_t *txt;
    size_t len = 0;
    JSValue r;
    int si;

    (void)magic;
    if (!n) return JS_NULL;
    if (node_is_chardata(n))
        return js_cd_get_data(ctx, this_val, 1);
    if (!node_has_children_as_text(n))
        return JS_NULL;                        /* §4.4 "otherwise: null" — a Document's is null, not "" */
    if (n->type == LXB_DOM_NODE_TYPE_ELEMENT) {
        si = attr_shadow_find(lxb_dom_interface_element(n), ATTR_SLOT_PROPERTY, "textContent");
        if (si >= 0)
            return JS_DupValue(ctx, attr_shadow_opaque(si));
    }
    txt = lxb_dom_node_text_content(n, &len);
    if (!txt) return JS_NewStringLen(ctx, "", 0);
    r = JS_NewStringLen(ctx, (const char *)txt, len);
    lxb_dom_document_destroy_text(n->owner_document, txt);
    return r;
}

static JSValue js_node_set_text_content(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic)
{
    lxb_dom_node_t *n = node_of(this_val), *node, *next;
    lxb_dom_text_t *text;
    const char *str;
    size_t len = 0;
    bool owned_cstr = false;

    (void)magic;
    if (!n) return JS_UNDEFINED;
    if (node_is_chardata(n))
        return js_cd_set_data(ctx, this_val, val, 1);   /* §4.4 CharacterData: "replace data" */
    if (!node_has_children_as_text(n))
        return JS_UNDEFINED;                            /* §4.4 "otherwise: do nothing" */

    if (n->type == LXB_DOM_NODE_TYPE_ELEMENT)
        dom_cow_set_prop_taint(ctx, lxb_dom_interface_element(n), "textContent",
                               concolic_is(val) ? val : JS_UNDEFINED);

    if (JS_IsNull(val)) {
        str = "";                     /* `DOMString?`: null is the empty string for "string replace all" */
    } else if (concolic_is(val)) {
        /* Unknown external input has no bytes: the SHAPE is what the Text node carries while the shadow above
           carries the value. */
        str = concolic_shape_c(val);
        if (!str) str = "";
        len = strlen(str);
    } else {
        DCHECK(JS_IsString(val), "textContent= reached the body unconverted — the IDL declaration is what "
                                 "converts it, and running the page's toString from here is the "
                                 "drive-to-completion the flow machinery exists to avoid");
        str = JS_ToCStringLen(ctx, &len, val);
        if (!str) return JS_EXCEPTION;
        owned_cstr = true;
    }

    /* §4.4 "string replace all": every child goes — through the per-flow chokepoint, so a forked arm that sets
       different text reads back its own — and then ONE Text node, ONLY IF the string is not empty. Appending an
       empty Text node unconditionally gave `el.textContent = ""` a child the spec says it has none of, which a
       page checking `firstChild` or `childNodes.length` reads as a tree it never built. */
    for (node = n->first_child; node; node = next) {
        next = node->next;
        dom_cow_remove_child(node);
    }
    if (len) {
        text = lxb_dom_document_create_text_node(n->owner_document, (const lxb_char_t *)str, len);
        DCHECK(text != NULL, "textContent= produced no Text node — the page's text would silently not be there");
        if (text)
            dom_cow_append_child(n, lxb_dom_interface_node(text));
    }
    if (owned_cstr)
        JS_FreeCString(ctx, str);
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry js_chardata_base[] = {
    JS_CGETSET_DEF("length", js_cd_get_length, NULL),
};

JSValueConst node_proto(void)
{
    DCHECK(g_protos_ready, "Node.prototype was asked for before node_init built it");
    return g_node_proto;
}

void node_set_proto(JSContext *ctx, int node_type, JSValue proto)
{
    DCHECK(g_protos_ready, "a derived interface registered its prototype before node_init built Node.prototype");
    DCHECK(node_type > 0 && node_type < LXB_DOM_NODE_TYPE_LAST_ENTRY,
           "a prototype was registered for a node type the DOM does not have");
    DCHECK(JS_VALUE_GET_PTR(g_protos[node_type]) == JS_VALUE_GET_PTR(g_node_proto),
           "two components claimed the same node type's interface — one of them would silently lose");
    JS_FreeValue(ctx, g_protos[node_type]);
    g_protos[node_type] = proto;        /* CONSUMED: the table owns every prototype it holds */
}

JSValue node_wrap(JSContext *ctx, lxb_dom_node_t *n)
{
    JSValue obj;
    int i;

    if (!n)
        return JS_NULL;
    for (i = 0; i < g_wrap_n; i++)
        if (g_wraps[i].n == n)
            return JS_DupValue(ctx, g_wraps[i].obj);

    DCHECK(g_protos_ready, "a node was wrapped before the DOM interfaces existed");
    DCHECK((int)n->type > 0 && (int)n->type < LXB_DOM_NODE_TYPE_LAST_ENTRY,
           "a Lexbor node carries a type the DOM does not define");
    DCHECK(n->type != LXB_DOM_NODE_TYPE_ELEMENT ||
           JS_VALUE_GET_PTR(g_protos[n->type]) != JS_VALUE_GET_PTR(g_node_proto),
           "an Element node was wrapped before element.c registered Element.prototype");
    obj = JS_NewObjectProtoClass(ctx, g_protos[n->type], g_node_class);
    if (JS_IsException(obj))
        return obj;
    JS_SetOpaque(obj, n);

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
    JSValue cd;
    int i, id_nodevalue, id_textcontent, id_data;

    if (g_protos_ready)
        return;    /* element.c asks for the base before building Element.prototype on top of it */

    JS_NewClassID(JS_GetRuntime(ctx), &g_node_class);
    JS_NewClass(JS_GetRuntime(ctx), g_node_class, &def);

    /* Node.prototype. §4.4 `interface Node : EventTarget`: every node is one, and only the global was — so
       `el.addEventListener(...)` was "not a function" on every element a page wired up, which is where
       testharness.js stopped on eight documents (`getElementById("rerun").addEventListener`). It belongs here
       because it is a BASE member, and here it is one function shared by every node rather than a fresh closure
       on each. */
    g_node_proto = JS_NewObject(ctx);
    CHECK(!JS_IsException(g_node_proto), "Node.prototype could not be allocated");
    JS_SetPropertyFunctionList(ctx, g_node_proto, js_node_base,
                               (int)(sizeof(js_node_base) / sizeof(js_node_base[0])));
    event_target_install(ctx, g_node_proto);
    g_protos_ready = 1;

    /* Every node kind is a Node until a component claims it. A ProcessingInstruction wrapper answering the Node
       members is honest; a bare object answering none of them is not. */
    for (i = 0; i < LXB_DOM_NODE_TYPE_LAST_ENTRY; i++)
        g_protos[i] = JS_DupValue(ctx, g_node_proto);

    /* `attribute DOMString? nodeValue` and `attribute DOMString? textContent` — both Node members, so both are
       declared and installed here. */
    id_nodevalue = idl_setter_id(ctx, IDL_DOMSTRING_NULLABLE, false, js_cd_set_data, 1);
    idl_install_accessor(ctx, g_node_proto, "nodeValue", js_cd_get_data, 1, id_nodevalue);
    id_textcontent = idl_setter_id(ctx, IDL_DOMSTRING_NULLABLE, false, js_node_set_text_content, 0);
    idl_install_accessor(ctx, g_node_proto, "textContent", js_node_get_text_content, 0, id_textcontent);

    /* CharacterData.prototype — §4.10, `interface CharacterData : Node`, so it INHERITS from Node.prototype
       rather than repeating its members. */
    cd = JS_NewObjectProto(ctx, g_node_proto);
    CHECK(!JS_IsException(cd), "CharacterData.prototype could not be allocated");
    JS_SetPropertyFunctionList(ctx, cd, js_chardata_base,
                               (int)(sizeof(js_chardata_base) / sizeof(js_chardata_base[0])));
    /* `attribute [LegacyNullToEmptyString] DOMString data` — the extended attribute is part of the TYPE, so the
       body never sees a null and never has to remember the rule. */
    id_data = idl_setter_id(ctx, IDL_DOMSTRING, true, js_cd_set_data, 0);
    idl_install_accessor(ctx, cd, "data", js_cd_get_data, 0, id_data);
    node_set_proto(ctx, LXB_DOM_NODE_TYPE_TEXT, JS_DupValue(ctx, cd));
    node_set_proto(ctx, LXB_DOM_NODE_TYPE_COMMENT, cd);
}

void node_free(JSContext *ctx)
{
    int i;
    for (i = 0; i < g_wrap_n; i++)
        JS_FreeValue(ctx, g_wraps[i].obj);
    free(g_wraps);
    g_wraps = NULL; g_wrap_n = g_wrap_cap = 0;
    /* Each derived prototype is held once per node type that maps to it; the base is held by the table too. */
    for (i = 0; i < LXB_DOM_NODE_TYPE_LAST_ENTRY; i++) {
        JS_FreeValue(ctx, g_protos[i]);
        g_protos[i] = JS_UNDEFINED;
    }
    JS_FreeValue(ctx, g_node_proto);
    g_node_proto = JS_UNDEFINED;
    g_protos_ready = 0;
}
