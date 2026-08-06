/* THE Attr INTERFACE — DOM §4.9.2, and the NamedNodeMap §4.9.1 that holds them.
 *
 * `el.attributes` was absent, and so was every object behind it. That is a gap with a particular shape: the
 * loops a page writes over an element's attributes — copying them onto a clone, mirroring `data-*` onto a
 * component, deciding which to forward — are `for (const a of el.attributes)`, and with no attributes there is
 * no iteration and no error either. The loop body simply never runs, and the engine reports whatever the page
 * does without it.
 *
 * AN Attr IS A NODE. Lexbor already models it as one (LXB_DOM_NODE_TYPE_ATTRIBUTE), and this engine already
 * has one wrapper table keyed by node — so an Attr gets identity for free, and `el.attributes[0] ===
 * el.attributes[0]` holds for the same reason `el.firstChild === el.firstChild` does. What was missing was a
 * PROTOTYPE for that node type: node_wrap handed an attribute Node.prototype, so it answered nodeType 2 while
 * `name` and `value` were undefined. Registering the interface is the whole fix.
 *
 * A NamedNodeMap IS AN INDEXED INTERFACE with a named getter beside it, which is exactly what idl_indexed
 * already is — so this file declares the two backings and none of the lookup, the descriptor, the key
 * enumeration, `in`, or @@iterator.
 *
 * §4.9.2 SAYS Attr's value SETTER IS "set an existing attribute value", which is the same change steps
 * setAttribute runs — so it goes through the mutation chokepoint and carries taint, exactly as
 * `el.setAttribute` does. An attribute reached through `el.attributes.href` and written there is a per-flow
 * write like any other, or a forked arm reads its sibling's value. */
#include <string.h>

#include <lexbor/dom/dom.h>
#include <lexbor/ns/ns.h>

#include "check.h"
#include "quickjs.h"
#include "solver/attr_shadow.h"
#include "solver/concolic.h"
#include "solver/dom_cow.h"
#include "core/dom/attr.h"
#include "core/dom/element.h"
#include "core/dom/node.h"
#include "core/idl_args.h"
#include "core/idl_indexed.h"

static JSValue g_attr_proto = JS_UNDEFINED;
static JSValue g_nnm_proto = JS_UNDEFINED;
static JSValue g_nnm_key = JS_UNDEFINED;    /* the map's owner element, under a private Symbol */
static int     g_ready;

static lxb_dom_attr_t *attr_of(JSValueConst v)
{
    lxb_dom_node_t *n = node_of(v);
    if (!n || n->type != LXB_DOM_NODE_TYPE_ATTRIBUTE) return NULL;
    return lxb_dom_interface_attr(n);
}

/* magic 0 = name, 1 = localName, 2 = value, 3 = namespaceURI, 4 = prefix, 5 = specified */
static JSValue js_attr_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    lxb_dom_attr_t *a = attr_of(this_val);
    const lxb_char_t *s;
    size_t len = 0;

    if (!a) return JS_UNDEFINED;
    switch (magic) {
    case 0: s = lxb_dom_attr_qualified_name(a, &len); break;
    case 1: s = lxb_dom_attr_local_name(a, &len); break;
    case 2: {
        /* THE TAINT SHADOW ANSWERS FIRST, as it does for getAttribute — a source written into this attribute
           came back out of Lexbor as plain bytes with its provenance gone, and a sink reading it looks clean. */
        int si;
        const lxb_char_t *qn = lxb_dom_attr_qualified_name(a, &len);
        if (a->owner && qn) {
            char *nm = malloc(len + 1);
            CHECK(nm != NULL, "Attr.value could not copy its own name");
            memcpy(nm, qn, len); nm[len] = 0;
            si = attr_shadow_find(a->owner, ATTR_SLOT_ATTRIBUTE, nm);
            free(nm);
            if (si >= 0) return JS_DupValue(ctx, attr_shadow_opaque(si));
        }
        s = lxb_dom_attr_value(a, &len);
        return s ? JS_NewStringLen(ctx, (const char *)s, len) : JS_NewStringLen(ctx, "", 0);
    }
    case 3:
        /* §4.9.2: an attribute with no namespace answers null, not the empty string. */
        return a->node.ns == LXB_NS__UNDEF || a->node.ns == LXB_NS__UNDEF
             ? JS_NULL : JS_NewString(ctx, "http://www.w3.org/1999/xhtml");
    case 4: return JS_NULL;    /* no namespace-aware setter exists yet, so no attribute here has a prefix */
    default:
        /* §4.9.2: `specified` is a historical member that is always true — the spec says so in as many words,
           and a value invented for it would be a different thing wearing the name. */
        return JS_TRUE;
    }
    return s ? JS_NewStringLen(ctx, (const char *)s, len) : JS_NewStringLen(ctx, "", 0);
}

static JSValue js_attr_owner(JSContext *ctx, JSValueConst this_val, int magic)
{
    lxb_dom_attr_t *a = attr_of(this_val);
    (void)magic;
    return (a && a->owner) ? element_wrap(ctx, a->owner) : JS_NULL;
}

/* §4.9.2 `attribute DOMString value` — "set an existing attribute value", which is setAttribute's change steps,
   so it goes through the same chokepoint and the same taint shadow. */
static JSValue js_attr_set_value(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic)
{
    lxb_dom_attr_t *a = attr_of(this_val);
    const lxb_char_t *qn;
    size_t len = 0;
    char *name;

    (void)magic;
    if (!a || !a->owner) return JS_UNDEFINED;
    qn = lxb_dom_attr_qualified_name(a, &len);
    if (!qn) return JS_UNDEFINED;
    name = malloc(len + 1);
    CHECK(name != NULL, "Attr.value could not copy its own name");
    memcpy(name, qn, len); name[len] = 0;

    if (concolic_is(val)) {
        const char *shape = concolic_shape_c(val);
        attr_shadow_set(ctx, a->owner, ATTR_SLOT_ATTRIBUTE, name, val);
        dom_cow_set_attribute(a->owner, name, shape ? shape : "", shape ? strlen(shape) : 0);
    } else {
        const char *s;
        attr_shadow_set(ctx, a->owner, ATTR_SLOT_ATTRIBUTE, name, JS_UNDEFINED);
        s = JS_ToCString(ctx, val);
        if (s) {
            dom_cow_set_attribute(a->owner, name, s, strlen(s));
            JS_FreeCString(ctx, s);
        }
    }
    free(name);
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry js_attr_reads[] = {
    JS_CGETSET_MAGIC_DEF("name", js_attr_get, NULL, 0),
    JS_CGETSET_MAGIC_DEF("localName", js_attr_get, NULL, 1),
    JS_CGETSET_MAGIC_DEF("namespaceURI", js_attr_get, NULL, 3),
    JS_CGETSET_MAGIC_DEF("prefix", js_attr_get, NULL, 4),
    JS_CGETSET_MAGIC_DEF("specified", js_attr_get, NULL, 5),
    JS_CGETSET_MAGIC_DEF("ownerElement", js_attr_owner, NULL, 0),
};

/* ---- NamedNodeMap §4.9.1 ------------------------------------------------------------------------------- */

/* The element this map is over. Held on the map under a private Symbol, the way a collection holds its owner. */
static lxb_dom_element_t *nnm_owner(JSContext *ctx, JSValueConst self)
{
    JSAtom k;
    JSValue owner;
    lxb_dom_node_t *n;

    if (!JS_IsObject(self)) return NULL;
    k = JS_ValueToAtom(ctx, g_nnm_key);
    if (k == JS_ATOM_NULL) return NULL;
    if (JS_GetOwnSlot(ctx, &owner, self, k) <= 0) owner = JS_UNDEFINED;
    JS_FreeAtom(ctx, k);
    n = node_of(owner);
    JS_FreeValue(ctx, owner);
    return (n && n->type == LXB_DOM_NODE_TYPE_ELEMENT) ? lxb_dom_interface_element(n) : NULL;
}

static uint32_t nnm_length(JSContext *ctx, JSValueConst self)
{
    lxb_dom_element_t *el = nnm_owner(ctx, self);
    lxb_dom_attr_t *a;
    uint32_t n = 0;

    if (!el) return 0;
    for (a = lxb_dom_element_first_attribute(el); a; a = lxb_dom_element_next_attribute(a)) n++;
    return n;
}

static JSValue nnm_item(JSContext *ctx, JSValueConst self, uint32_t i)
{
    lxb_dom_element_t *el = nnm_owner(ctx, self);
    lxb_dom_attr_t *a;
    uint32_t k = 0;

    if (!el) return JS_UNDEFINED;
    for (a = lxb_dom_element_first_attribute(el); a; a = lxb_dom_element_next_attribute(a), k++)
        if (k == i) return node_wrap(ctx, lxb_dom_interface_node(a));
    return JS_UNDEFINED;
}

/* §4.9.1's NAMED getter — `el.attributes.href` is the Attr, which is how a great deal of older code reads one. */
static JSValue nnm_named(JSContext *ctx, JSValueConst self, const char *name)
{
    lxb_dom_element_t *el = nnm_owner(ctx, self);
    lxb_dom_attr_t *a;
    size_t nlen = strlen(name), qlen = 0;

    if (!el) return JS_UNDEFINED;
    for (a = lxb_dom_element_first_attribute(el); a; a = lxb_dom_element_next_attribute(a)) {
        const lxb_char_t *q = lxb_dom_attr_qualified_name(a, &qlen);
        if (q && qlen == nlen && memcmp(q, name, nlen) == 0)
            return node_wrap(ctx, lxb_dom_interface_node(a));
    }
    return JS_UNDEFINED;
}

static const IdlIndexedDecl NNM_INDEXED = { "NamedNodeMap", nnm_length, nnm_item, nnm_named, 0 };

static JSValue js_nnm_length(JSContext *ctx, JSValueConst this_val, int magic)
{
    (void)magic;
    return JS_NewUint32(ctx, nnm_length(ctx, this_val));
}

/* §4.9.1 item(index) / getNamedItem(name) — null past the end or for a name that is not there, which is what
   makes them different from the two getters above. magic 0 = item, 1 = getNamedItem. */
static JSValue js_nnm_get(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    JSValue r;

    if (argc < 1) return JS_NULL;
    if (magic == 0) {
        int64_t i = 0;
        JS_ToInt64(ctx, &i, argv[0]);   /* a real number by now: the declaration converted it */
        r = i < 0 ? JS_UNDEFINED : nnm_item(ctx, this_val, (uint32_t)i);
    } else {
        const char *name = JS_ToCString(ctx, argv[0]);   /* a real string by now */
        if (!name) return JS_EXCEPTION;
        r = nnm_named(ctx, this_val, name);
        JS_FreeCString(ctx, name);
    }
    return JS_IsUndefined(r) ? JS_NULL : r;
}

/* §4.9.1 removeNamedItem — a name that is not there is a NotFoundError, not a quiet no-op. */
static JSValue js_nnm_remove(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    lxb_dom_element_t *el = nnm_owner(ctx, this_val);
    const char *name;
    JSValue found;

    (void)magic;
    if (!el || argc < 1) return JS_NULL;
    name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_EXCEPTION;
    found = nnm_named(ctx, this_val, name);
    if (JS_IsUndefined(found)) {
        JS_FreeCString(ctx, name);
        return JS_ThrowDOMException(ctx, "NotFoundError", "no attribute of that name");
    }
    attr_shadow_set(ctx, el, ATTR_SLOT_ATTRIBUTE, name, JS_UNDEFINED);
    dom_cow_remove_attribute(el, name);   /* the removal chokepoint, so it reverts per flow */
    JS_FreeCString(ctx, name);
    return found;
}

JSValue attr_named_node_map_new(JSContext *ctx, JSValueConst owner)
{
    JSValue obj = idl_indexed_new(ctx, g_nnm_proto, &NNM_INDEXED);
    JSAtom k;

    DCHECK(g_ready, "a NamedNodeMap was built before attr_init ran");
    if (JS_IsException(obj)) return obj;
    k = JS_ValueToAtom(ctx, g_nnm_key);
    CHECK(k != JS_ATOM_NULL, "the NamedNodeMap owner key could not be reached");
    JS_DefinePropertyValue(ctx, obj, k, JS_DupValue(ctx, owner), 0);
    JS_FreeAtom(ctx, k);
    return obj;
}

void attr_init(JSContext *ctx)
{
    static const IdlArgType ONE_LONG[1] = { IDL_LONG };
    static const IdlArgType ONE_STR[1] = { IDL_DOMSTRING };

    DCHECK(!g_ready, "attr_init ran twice — one instance is one document");
    g_nnm_key = JS_NewSymbol(ctx, "namedNodeMapOwner", false);
    CHECK(!JS_IsException(g_nnm_key), "the NamedNodeMap owner key could not be allocated");

    /* §4.9.2 `interface Attr : Node` — so it inherits Node's members rather than repeating them, and node_wrap
       hands an attribute THIS from now on instead of the bare Node it was giving. */
    g_attr_proto = JS_NewObjectProto(ctx, node_proto());
    CHECK(!JS_IsException(g_attr_proto), "Attr.prototype could not be allocated");
    idl_interface_tag(ctx, g_attr_proto, "Attr");
    JS_SetPropertyFunctionList(ctx, g_attr_proto, js_attr_reads,
                               (int)(sizeof(js_attr_reads) / sizeof(js_attr_reads[0])));
    idl_install_accessor(ctx, g_attr_proto, "value", js_attr_get, 2,
                         idl_setter_id(ctx, IDL_DOMSTRING, false, js_attr_set_value, 0));
    node_set_proto(ctx, LXB_DOM_NODE_TYPE_ATTRIBUTE, JS_DupValue(ctx, g_attr_proto));

    g_nnm_proto = JS_NewObject(ctx);
    CHECK(!JS_IsException(g_nnm_proto), "NamedNodeMap.prototype could not be allocated");
    idl_interface_tag(ctx, g_nnm_proto, "NamedNodeMap");
    idl_install_accessor(ctx, g_nnm_proto, "length", js_nnm_length, 0, -1);
    idl_install_method(ctx, g_nnm_proto, "item", 1, idl_method_id(ctx, ONE_LONG, 1, js_nnm_get, 0));
    idl_install_method(ctx, g_nnm_proto, "getNamedItem", 1,
                       idl_method_id(ctx, ONE_STR, 1, js_nnm_get, 1));
    idl_install_method(ctx, g_nnm_proto, "removeNamedItem", 1,
                       idl_method_id(ctx, ONE_STR, 1, js_nnm_remove, 0));
    /* §3.7.10: an interface with an indexed getter gets %Array.prototype.values% as its @@iterator, which is
       what makes `for (const a of el.attributes)` — the loop this gap was really about — ordinary code. */
    idl_indexed_install_iterable(ctx, g_nnm_proto);
    g_ready = 1;
}

void attr_install(JSContext *ctx, JSValueConst global)
{
    DCHECK(g_ready, "the Attr interfaces were installed before their prototypes were built");
    node_install_interface(ctx, global, "Attr", g_attr_proto);
    {
        JSValue nnm = JS_NewCFunction2(ctx, NULL, "NamedNodeMap", 0, JS_CFUNC_constructor, 0);
        CHECK(!JS_IsException(nnm), "the NamedNodeMap interface object could not be allocated");
        JS_SetConstructor(ctx, nnm, g_nnm_proto);
        JS_SetPropertyStr(ctx, (JSValue)global, "NamedNodeMap", nnm);
    }
}

void attr_free(JSContext *ctx)
{
    if (!g_ready) return;
    JS_FreeValue(ctx, g_attr_proto);
    JS_FreeValue(ctx, g_nnm_proto);
    JS_FreeValue(ctx, g_nnm_key);
    g_attr_proto = g_nnm_proto = g_nnm_key = JS_UNDEFINED;
    g_ready = 0;
}
