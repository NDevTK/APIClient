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
#include <stdlib.h>
#include <string.h>

#include <lexbor/dom/dom.h>
#include <lexbor/ns/ns.h>

#include "check.h"
#include "quickjs.h"
#include "solver/attr_shadow.h"
#include "solver/concolic.h"
#include "solver/dom_cow.h"
#include "core/dom/attr.h"
#include "core/dom/attr_list.h"
#include "core/dom/element.h"
#include "core/dom/names.h"
#include "core/dom/node.h"
#include "core/html/trusted_types.h"
#include "core/idl_args.h"
#include "core/realm.h"
#include "core/idl_indexed.h"

/* PER REALM — §3.7. Held in quickjs's per-context class-proto slots; the node-type table names the CLASS. */
static JSClassID g_attr_class, g_nnm_class;
/* Declared once per AGENT (the IDL pool is sealed after agent init); installed per realm. */
static int g_set_value_id = -1, g_item_id = -1, g_get_named_id = -1, g_remove_named_id = -1,
           g_get_named_ns_id = -1, g_remove_named_ns_id = -1, g_set_named_id = -1,
           g_create_attr_id = -1, g_create_attr_ns_id = -1;
static JSValue g_nnm_key = JS_UNDEFINED;    /* the map's owner element, under a private Symbol */
static int     g_ready;

lxb_dom_attr_t *attr_node_of(JSValueConst v)
{
    lxb_dom_node_t *n = node_of(v);
    if (!n || n->type != LXB_DOM_NODE_TYPE_ATTRIBUTE) return NULL;
    return lxb_dom_interface_attr(n);
}

/* THIS ATTRIBUTE'S OWN §4.9 IDENTITY — (namespace, LOCAL name), as the NUL-terminated strings the chokepoint
 * takes. `ns` is NULL for the null namespace.
 *
 * NOT THE QUALIFIED NAME, which is what this file used to reach the chokepoint with, and the difference is a
 * write landing on a DIFFERENT ATTRIBUTE. §9.11 Q3: `el.setAttribute("xlink:href", v)` creates a NULL-namespace
 * attribute whose local name is the whole ten-character string, which legitimately coexists with a
 * parser-produced XLink `href` wearing the same qualified name — and "get an attribute by name" answers with
 * whichever of the two comes FIRST. So `el.getAttributeNode("xlink:href").value = v` reached by qualified name
 * could read one attribute's taint and write the other's value.
 *
 * OWNED COPIES, no fixed buffer: lexbor's intern table stores a string with its length beside it and
 * NUL-terminates only the short ones, and an attribute's name length is the page's data. */
static char *attr_dupn(const lxb_char_t *s, size_t n)
{
    char *p = malloc(n + 1);
    CHECK(p != NULL, "an attribute identity could not be copied");
    memcpy(p, s, n); p[n] = 0;
    return p;
}

void attr_key_of(const lxb_dom_attr_t *a, AttrKey *k)
{
    const lxb_char_t *s;
    size_t len = 0;

    k->ns = NULL;
    if ((s = dom_attr_ns(a, &len)) != NULL) k->ns = attr_dupn(s, len);
    len = 0;
    s = lxb_dom_attr_local_name((lxb_dom_attr_t *)a, &len);
    DCHECK(s != NULL, "an attribute in the tree carries no local name — §4.9.1 says a local name is a "
                      "non-empty string, so there is nothing this could legitimately be");
    k->local = attr_dupn(s, len);
}

void attr_key_free(AttrKey *k) { free(k->ns); free(k->local); k->ns = k->local = NULL; }

/* magic 0 = name, 1 = localName, 2 = value, 3 = namespaceURI, 4 = prefix, 5 = specified */
static JSValue js_attr_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    lxb_dom_attr_t *a = attr_node_of(this_val);
    const lxb_char_t *s;
    size_t len = 0;

    if (!a) return JS_UNDEFINED;
    switch (magic) {
    case 0: s = lxb_dom_attr_qualified_name(a, &len); break;
    case 1: s = lxb_dom_attr_local_name(a, &len); break;
    case 2: {
        /* THE TAINT SHADOW ANSWERS FIRST, as it does for getAttribute — a source written into this attribute
           came back out of Lexbor as plain bytes with its provenance gone, and a sink reading it looks clean.
           Asked at THIS attribute's own identity, so a namespaced attribute does not answer with the taint of
           the null-namespace one that happens to print the same qualified name. */
        {
            AttrKey k;
            int si;

            attr_key_of(a, &k);
            /* AN ATTRIBUTE WITH NO ELEMENT KEYS ITS SHADOW ON ITSELF (attr_shadow.h): `createAttribute` returns
               one that never had an element and `removeAttributeNode` hands one back, and a source written into
               either is still a source. */
            si = attr_shadow_find(a->owner ? (const void *)a->owner : (const void *)a,
                                  ATTR_SLOT_ATTRIBUTE, k.ns, k.local);
            attr_key_free(&k);
            if (si >= 0) return JS_DupValue(ctx, attr_shadow_opaque(si));
        }
        s = lxb_dom_attr_value(a, &len);
        return s ? JS_NewStringLen(ctx, (const char *)s, len) : JS_NewStringLen(ctx, "", 0);
    }
    case 3:
        /* §4.9.2: an attribute with no namespace answers null, not the empty string — and the namespace it
           does have is the one the attribute carries, read back out of the document's table. This answered a
           hardcoded XHTML URI for every attribute lexbor had marked with its element's namespace, which is
           every scripted `setAttribute` on an HTML element; the writes now record the null namespace §4.9 says
           they are in, so there is a real fact here to report. */
        s = dom_attr_ns(a, &len);
        return s ? JS_NewStringLen(ctx, (const char *)s, len) : JS_NULL;
    case 4:
        s = dom_attr_prefix(a, &len);
        return s ? JS_NewStringLen(ctx, (const char *)s, len) : JS_NULL;
    default:
        /* §4.9.2: `specified` is a historical member that is always true — the spec says so in as many words,
           and a value invented for it would be a different thing wearing the name. */
        return JS_TRUE;
    }
    return s ? JS_NewStringLen(ctx, (const char *)s, len) : JS_NewStringLen(ctx, "", 0);
}

static JSValue js_attr_owner(JSContext *ctx, JSValueConst this_val, int magic)
{
    lxb_dom_attr_t *a = attr_node_of(this_val);
    (void)magic;
    return (a && a->owner) ? element_wrap(ctx, a->owner) : JS_NULL;
}

/* §4.9.2 `attribute DOMString value` — "set an existing attribute value", which is setAttribute's change steps,
   so it goes through the same chokepoint and the same taint shadow. */
static JSValue js_attr_set_value(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic)
{
    lxb_dom_attr_t *a = attr_node_of(this_val);
    AttrKey k;
    const char *bytes, *owned = NULL;
    size_t len;
    JSValueConst taint;

    (void)magic;
    if (!a) return JS_UNDEFINED;
    /* A concolic value has no bytes to store: its SHAPE goes into the tree and the value itself into the
       shadow, which is what makes the read give the same concolic back. */
    if (concolic_is(val)) {
        const char *shape = concolic_shape_c(val);
        bytes = shape ? shape : ""; len = shape ? strlen(shape) : 0; taint = val;
    } else {
        owned = JS_ToCString(ctx, val);   /* a real string by now: the declaration converted it */
        if (!owned) return JS_EXCEPTION;
        bytes = owned; len = strlen(owned); taint = JS_UNDEFINED;
    }
    /* §4.9.2 step 5's "change an attribute" is over THIS attribute, so the write is keyed on its own
       (namespace, local name) — the prefix is never consulted, because changing a value does not rename. */
    attr_key_of(a, &k);
    /* §9.4.10 STEPS 1 AND 4, which are the same test either side of the Trusted Types call: an attribute whose
       element is NULL has its value set and nothing else — no change steps, no element to run them on. */
    if (a->owner) dom_cow_set_attribute_ns(a->owner, k.ns, NULL, k.local, bytes, len, taint);
    else dom_cow_set_detached_attr_value(a, bytes, len, taint);
    attr_key_free(&k);
    if (owned) JS_FreeCString(ctx, owned);
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
JSValue attr_by_name(JSContext *ctx, lxb_dom_element_t *el, const char *name)
{
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

static JSValue nnm_named(JSContext *ctx, JSValueConst self, const char *name)
{
    return attr_by_name(ctx, nnm_owner(ctx, self), name);
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

/* §4.9.1's (NAMESPACE, LOCAL NAME) KEY SPACE — `getNamedItemNS` and `removeNamedItemNS`, which are §4.9's two
   namespace-keyed primitives reached through the map instead of through the element. One body because what is
   easy to get wrong is the KEY (step 1's "the empty string is the null namespace", and THE attribute rather
   than the first), not the answer. magic 0 = getNamedItemNS, 1 = removeNamedItemNS. */
static JSValue js_nnm_ns(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    lxb_dom_element_t *el = nnm_owner(ctx, this_val);
    const char *ns, *local;
    lxb_dom_attr_t *a;
    JSValue r;

    if (!el || argc < 2) return JS_NULL;
    ns = JS_IsNull(argv[0]) ? NULL : JS_ToCString(ctx, argv[0]);   /* a real string by now: the declaration converted it */
    if (ns && !ns[0]) { JS_FreeCString(ctx, ns); ns = NULL; }      /* step 1: "" IS the null namespace */
    local = JS_ToCString(ctx, argv[1]);
    if (!local) { if (ns) JS_FreeCString(ctx, ns); return JS_EXCEPTION; }
    a = dom_attr_get_ns(el, ns, local);
    if (!a) {
        /* §4.9.1: `removeNamedItemNS` throws NotFoundError where `removeAttributeNS` is silent — the one
           behavioural difference between this surface and Element's over the same two primitives. */
        r = magic == 0 ? JS_NULL
                       : JS_ThrowDOMException(ctx, "NotFoundError", "no attribute at that namespace and local name");
    } else {
        r = node_wrap(ctx, lxb_dom_interface_node(a));   /* the Attr, which §9.4.7 leaves alive and detached */
        if (magic == 1) dom_cow_remove_attribute_node(a);
    }
    JS_FreeCString(ctx, local);
    if (ns) JS_FreeCString(ctx, ns);
    return r;
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
    /* THE NODE, not the name: §9.4.3 finds the attribute by qualified name and then removes THAT ATTRIBUTE, and
       the object this returns has to be the one that was removed. */
    dom_cow_remove_attribute_node(attr_node_of(found));
    JS_FreeCString(ctx, name);
    return found;
}


JSClassID attr_class_id(void) { return g_attr_class; }
lxb_dom_element_t *attr_named_node_map_owner(JSContext *ctx, JSValueConst map) { return nnm_owner(ctx, map); }

/* §4.9 "SET AN ATTRIBUTE" (§9.4.1), AS A MACHINE, and ONE machine for FOUR members: `setAttributeNode`,
 * `setAttributeNodeNS`, `setNamedItem` and `setNamedItemNS` are that algorithm in one sentence each, and the
 * `NS` suffix carries NO behavioural difference at all — both pairs key on (namespace, local name) through the
 * algorithm's own step 3. The magic says only where the element comes from: 0 = `this` IS the element, 1 =
 * `this` is the NamedNodeMap over it.
 *
 * IT IS A MACHINE BECAUSE STEP 1 IS THE TRUSTED TYPES CALL AND IT RUNS BEFORE STEP 2. A page's default policy
 * therefore runs even for a call that is about to throw InUseAttributeError, and the policy callback can move
 * `attr` onto another element — so step 2 must read `attr`'s element AFTER the resume, never a value captured
 * before step 1. That is §9.13 item 2, and it is the whole reason the boundary sits where it does. */
#define ATTR_SET_STAGES(X) \
    X(SETATTRNODE_TRUSTED, "DOM §4.9 \"set an attribute\" step 1 (get trusted type compliant attribute value " \
                           "with attr's local name, attr's namespace, element and attr's value), which runs " \
                           "BEFORE step 2's InUseAttributeError check") \
    X(SETATTRNODE_WRITE,   "DOM §4.9 \"set an attribute\" steps 2-8 (re-read attr's element for the " \
                           "InUseAttributeError, find the old attribute at (namespace, local name), set the " \
                           "verified value, replace or append, and return the old attribute)")
enum { IDL_STEP_STAGE_BASE(ATTR_SET_STAGES) ATTR_SET_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const ATTR_SET_STEPS[] = { ATTR_SET_STAGES(JS_STEP_STAGE_LABEL) NULL };

typedef struct { JSValue verified; } AttrSetState;

static void attr_set_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    AttrSetState *s = st;
    v->val(ctx, &s->verified);
}

static int js_attr_set_attribute(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                                 JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    AttrSetState *s = st;
    int magic = idl_step_magic(hdr);
    lxb_dom_element_t *el = magic == 0 ? element_of_value(hdr->this_val)
                                       : nnm_owner(ctx, hdr->this_val);
    lxb_dom_attr_t *a = argc > 0 ? attr_node_of(argv[0]) : NULL;
    AttrKey k;

    (void)out_cb; (void)out_argc;
    JS_FreeValue(ctx, cb_result);
    *presult = JS_NULL;
    if (!el || !a) return JS_STEP_DONE;

    if (hdr->stage == SETATTRNODE_TRUSTED) {
        char nsbuf[128], lobuf[64];
        const char *el_ns, *el_local;
        JSValue cur;
        size_t vl = 0;
        const lxb_char_t *v = lxb_dom_attr_value(a, &vl);

        element_ns_and_local(el, &el_ns, &el_local, nsbuf, sizeof(nsbuf), lobuf, sizeof(lobuf));
        attr_key_of(a, &k);
        cur = JS_NewStringLen(ctx, (const char *)(v ? v : (const lxb_char_t *)""), v ? vl : 0);
        s->verified = trusted_types_compliant_attribute_value(ctx, el_ns, el_local, k.ns, k.local, cur);
        JS_FreeValue(ctx, cur);
        attr_key_free(&k);
        if (JS_IsException(s->verified)) { s->verified = JS_UNDEFINED; return JS_STEP_ABRUPT; }
        hdr->stage = SETATTRNODE_WRITE;
    }
    DCHECK(hdr->stage == SETATTRNODE_WRITE, "set-an-attribute resumed into a stage it does not have");
    /* STEP 2, RE-READ. The policy callback at step 1 may have moved `attr` onto another element. */
    if (a->owner && a->owner != el) {
        JS_ThrowDOMException(ctx, "InUseAttributeError",
                             "the attribute is already an attribute of another element");
        return JS_STEP_ABRUPT;
    }
    attr_key_of(a, &k);
    {
        lxb_dom_attr_t *old = dom_attr_get_ns(el, k.ns, k.local);   /* step 3 */
        const char *val;

        if (old == a) {                                            /* step 4 — a no-op that still ran the policy */
            attr_key_free(&k);
            *presult = node_wrap(ctx, lxb_dom_interface_node(a));
            return JS_STEP_DONE;
        }
        *presult = old ? node_wrap(ctx, lxb_dom_interface_node(old)) : JS_NULL;   /* step 8, taken before step 6 */
        /* STEP 5 writes the attribute's OWN value back through Trusted Types, so the provenance it already
           carries survives it: a concolic verified value IS the taint, and a plain one keeps whatever the node
           was holding. Written with JS_UNDEFINED instead, `a.value = location.hash; el.setAttributeNode(a)`
           would clear the source at the very step that puts it into the DOM. */
        if (concolic_is(s->verified)) {
            const char *shape = concolic_shape_c(s->verified);
            dom_cow_set_detached_attr_value(a, shape ? shape : "", shape ? strlen(shape) : 0, s->verified);
        } else {
            int si = attr_shadow_find(a, ATTR_SLOT_ATTRIBUTE, k.ns, k.local);
            JSValue keep = si >= 0 ? JS_DupValue(ctx, attr_shadow_opaque(si)) : JS_UNDEFINED;

            val = JS_ToCString(ctx, s->verified);                  /* step 5 */
            DCHECK(val != NULL, "the verified attribute value reached the write unconverted");
            if (val) {
                dom_cow_set_detached_attr_value(a, val, strlen(val), keep);
                JS_FreeCString(ctx, val);
            }
            JS_FreeValue(ctx, keep);
        }
        dom_cow_put_attribute_node(el, a, JS_UNDEFINED);           /* steps 6 and 7 */
    }
    attr_key_free(&k);
    return JS_STEP_DONE;
}

static const IdlStepDecl ATTR_SET_STEP = {
    js_attr_set_attribute, sizeof(AttrSetState), attr_set_visit, NULL,
    "DOM §4.9 \"set an attribute\"", ATTR_SET_STEPS
};

const IdlStepDecl *attr_set_attribute_decl(void) { return &ATTR_SET_STEP; }


/* §4.5's `createAttribute(localName)` and `createAttributeNS(namespace, qualifiedName)` — §4.9.2's "create an
 * attribute" with §1.4's name check in front of it. They live BESIDE the interface they build rather than in
 * document.c, so there is one "create an attribute" and not a second copy of it three files away.
 *
 * THE LOWERCASING CONDITION HAS ONE TERM HERE AND ONLY HERE. `createAttribute` step 2 is "If THIS IS AN HTML
 * DOCUMENT, set localName to localName in ASCII lowercase" — no HTML-namespace conjunct, because there is no
 * element yet. Every other lowercasing site in the attribute surface (get an attribute by name, setAttribute,
 * hasAttribute, toggleAttribute, NamedNodeMap's supported property names) tests TWO terms, and
 * `createAttributeNS` tests none at all. magic 0 = createAttribute, 1 = createAttributeNS. */
static JSValue js_doc_create_attribute(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                                       int magic)
{
    lxb_dom_node_t *n = node_of(this_val);
    lxb_dom_document_t *doc;
    lxb_dom_attr_t *a;

    /* WEB IDL §3.7.5: a member reached with a receiver that does not implement the interface is a TypeError,
       thrown at the call. NOT a DCHECK — the corpus asks for that throw deliberately. */
    if (!n || n->type != LXB_DOM_NODE_TYPE_DOCUMENT) return JS_ThrowTypeError(ctx, "this is not a Document");
    doc = lxb_dom_interface_document(n);
    if (magic == 0) {
        const char *local = JS_ToCString(ctx, argv[0]);   /* a real string by now: the declaration converted it */
        char *lowered;
        size_t len, i;

        if (!local) return JS_EXCEPTION;
        len = strlen(local);
        if (!dom_valid_attribute_local_name(local, len)) {          /* step 1 */
            JS_FreeCString(ctx, local);
            return JS_ThrowDOMException(ctx, "InvalidCharacterError",
                                        "createAttribute was given a name that is not a valid attribute local name");
        }
        lowered = malloc(len + 1);
        CHECK(lowered != NULL, "createAttribute could not copy its own local name");
        memcpy(lowered, local, len + 1);
        JS_FreeCString(ctx, local);
        if (doc->type == LXB_DOM_DOCUMENT_DTYPE_HTML)               /* step 2 — ONE term */
            for (i = 0; i < len; i++)
                if (lowered[i] >= 'A' && lowered[i] <= 'Z') lowered[i] = (char)(lowered[i] - 'A' + 'a');
        a = dom_cow_create_attribute(doc, NULL, NULL, lowered);     /* step 3 */
        free(lowered);
    } else {
        const char *ns_arg, *qn;
        size_t ns_len = 0;
        DomQName ex;
        bool ok;

        DCHECK(magic == 1, "a document attribute factory was declared with a magic this file does not name");
        ns_arg = JS_IsNull(argv[0]) ? NULL : JS_ToCStringLen(ctx, &ns_len, argv[0]);
        qn = JS_ToCString(ctx, argv[1]);
        if (!qn) { if (ns_arg) JS_FreeCString(ctx, ns_arg); return JS_EXCEPTION; }
        ok = dom_validate_and_extract(ctx, ns_arg, ns_len, qn, strlen(qn), DOM_NAME_ATTRIBUTE, &ex);   /* step 1 */
        if (ok) {
            /* The three slices are borrowed halves of `qn` and `ns_arg`, so they are NUL-terminated here and
               nowhere else — "create an attribute" takes strings, and the colon is the separator. */
            char *nsz = NULL, *pfx = NULL, *loc;

            if (ex.ns) { nsz = malloc(ex.ns_len + 1); CHECK(nsz, "createAttributeNS could not copy its namespace");
                         memcpy(nsz, ex.ns, ex.ns_len); nsz[ex.ns_len] = 0; }
            if (ex.prefix) { pfx = malloc(ex.prefix_len + 1); CHECK(pfx, "createAttributeNS could not copy its prefix");
                             memcpy(pfx, ex.prefix, ex.prefix_len); pfx[ex.prefix_len] = 0; }
            loc = malloc(ex.local_len + 1); CHECK(loc, "createAttributeNS could not copy its local name");
            memcpy(loc, ex.local, ex.local_len); loc[ex.local_len] = 0;
            a = dom_cow_create_attribute(doc, nsz, pfx, loc);        /* step 2 */
            free(nsz); free(pfx); free(loc);
        } else {
            a = NULL;
        }
        JS_FreeCString(ctx, qn);
        if (ns_arg) JS_FreeCString(ctx, ns_arg);
        if (!ok) return JS_EXCEPTION;   /* §1.4 already threw the exception its step names */
    }
    return node_wrap(ctx, lxb_dom_interface_node(a));
}

void attr_install_document_members(JSContext *ctx, JSValueConst doc_proto)
{
    DCHECK(g_create_attr_id >= 0 && g_create_attr_ns_id >= 0,
           "Document's attribute factories were installed before they were declared");
    idl_install_method(ctx, (JSValue)doc_proto, "createAttribute", 1, g_create_attr_id);
    idl_install_method(ctx, (JSValue)doc_proto, "createAttributeNS", 2, g_create_attr_ns_id);
}

JSValue attr_named_node_map_new(JSContext *ctx, JSValueConst owner)
{
    JSValue nnm_p = named_node_map_proto(ctx);
    JSValue obj = idl_indexed_new(ctx, nnm_p, &NNM_INDEXED);
    JS_FreeValue(ctx, nnm_p);
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
    /* `(DOMString? namespace, DOMString localName)` — §4.9's namespace-keyed argument list. The namespace is
       declared NULLABLE rather than tested in the body: Web IDL turns null AND undefined into the IDL null
       before ToString is reached, so `getNamedItemNS(undefined, "x")` must not look for the namespace whose
       URL is the four characters `null`. */
    static const IdlArgType NS_LOCAL[2] = { IDL_DOMSTRING_NULLABLE, IDL_DOMSTRING };
    static const IdlArgType ONE_ATTR[1] = { IDL_INTERFACE };
    JSClassDef ad = { "Attr" }, nd = { "NamedNodeMap" };

    DCHECK(!g_ready, "attr_init ran twice — the interfaces are declared once per AGENT");
    g_nnm_key = JS_NewSymbol(ctx, "namedNodeMapOwner", false);
    CHECK(!JS_IsException(g_nnm_key), "the NamedNodeMap owner key could not be allocated");
    JS_NewClassID(JS_GetRuntime(ctx), &g_attr_class);
    JS_NewClass(JS_GetRuntime(ctx), g_attr_class, &ad);
    JS_NewClassID(JS_GetRuntime(ctx), &g_nnm_class);
    JS_NewClass(JS_GetRuntime(ctx), g_nnm_class, &nd);
    /* §4.9.2 `interface Attr : Node` — so it inherits Node's members rather than repeating them, and node_wrap
       hands an attribute THIS from now on instead of the bare Node it was giving. */
    node_claim_type(LXB_DOM_NODE_TYPE_ATTRIBUTE, g_attr_class);
    g_set_value_id = idl_setter_id(ctx, IDL_DOMSTRING, false, js_attr_set_value, 0);
    g_item_id = idl_method_id(ctx, ONE_LONG, 1, js_nnm_get, 0);
    g_get_named_id = idl_method_id(ctx, ONE_STR, 1, js_nnm_get, 1);
    g_remove_named_id = idl_method_id(ctx, ONE_STR, 1, js_nnm_remove, 0);
    g_get_named_ns_id = idl_method_id(ctx, NS_LOCAL, 2, js_nnm_ns, 0);
    g_remove_named_ns_id = idl_method_id(ctx, NS_LOCAL, 2, js_nnm_ns, 1);
    /* `[CEReactions] Attr? setNamedItem(Attr attr)` — the `Attr attr` position is an INTERFACE type, so
       anything that is not one is a TypeError thrown before step 1 rather than a check in the body. */
    g_set_named_id = idl_method_id_step(ctx, ONE_ATTR, 1, NULL, 0, &ATTR_SET_STEP, 1);
    idl_iface_brand(g_attr_class);
    g_create_attr_id = idl_method_id(ctx, ONE_STR, 1, js_doc_create_attribute, 0);
    g_create_attr_ns_id = idl_method_id(ctx, NS_LOCAL, 2, js_doc_create_attribute, 1);
    g_ready = 1;
    realm_declare_intrinsic(attr_install_protos);
}

/* §4.9's TWO INTERFACE PROTOTYPE OBJECTS, FOR ONE REALM. */
void attr_install_protos(JSContext *ctx)
{
    JSValue attr_p, nnm_p, base, prev;

    DCHECK(g_ready, "a realm asked for Attr.prototype before the interfaces were declared");
    prev = JS_GetClassProto(ctx, g_attr_class);
    DCHECK(JS_IsNull(prev), "attr_install_protos ran twice in one realm");
    JS_FreeValue(ctx, prev);
    base = node_proto(ctx);
    attr_p = JS_NewObjectProto(ctx, base);
    JS_FreeValue(ctx, base);
    CHECK(!JS_IsException(attr_p), "Attr.prototype could not be allocated");
    idl_interface_tag(ctx, attr_p, "Attr");
    JS_SetPropertyFunctionList(ctx, attr_p, js_attr_reads,
                               (int)(sizeof(js_attr_reads) / sizeof(js_attr_reads[0])));
    idl_install_accessor(ctx, attr_p, "value", js_attr_get, 2, g_set_value_id);
    JS_SetClassProto(ctx, g_attr_class, attr_p);

    nnm_p = JS_NewObject(ctx);
    CHECK(!JS_IsException(nnm_p), "NamedNodeMap.prototype could not be allocated");
    idl_interface_tag(ctx, nnm_p, "NamedNodeMap");
    idl_install_accessor(ctx, nnm_p, "length", js_nnm_length, 0, -1);
    idl_install_method(ctx, nnm_p, "item", 1, g_item_id);
    idl_install_method(ctx, nnm_p, "getNamedItem", 1, g_get_named_id);
    idl_install_method(ctx, nnm_p, "removeNamedItem", 1, g_remove_named_id);
    idl_install_method(ctx, nnm_p, "getNamedItemNS", 2, g_get_named_ns_id);
    idl_install_method(ctx, nnm_p, "removeNamedItemNS", 2, g_remove_named_ns_id);
    /* §4.9.1: `setNamedItem` and `setNamedItemNS` are the SAME algorithm in one sentence each — one machine,
       installed twice, because the IDL declares two members and a page may call either. */
    idl_install_method(ctx, nnm_p, "setNamedItem", 1, g_set_named_id);
    idl_install_method(ctx, nnm_p, "setNamedItemNS", 1, g_set_named_id);
    /* §3.7.10: an interface with an indexed getter gets %Array.prototype.values% as its @@iterator, which is
       what makes `for (const a of el.attributes)` — the loop this gap was really about — ordinary code. */
    idl_indexed_install_iterable(ctx, nnm_p);   /* §4.9.1 declares no iterable<>, so no value iterator */
    JS_SetClassProto(ctx, g_nnm_class, nnm_p);
}

JSValue named_node_map_proto(JSContext *ctx)
{
    JSValue proto = JS_GetClassProto(ctx, g_nnm_class);
    DCHECK(!JS_IsNull(proto), "NamedNodeMap.prototype was asked for in a realm that never ran its install");
    return proto;   /* OWNED */
}

void attr_install(JSContext *ctx, JSValueConst global)
{
    DCHECK(g_ready, "the Attr interfaces were installed before their prototypes were built");
    {
        JSValue attr_p = node_type_proto(ctx, LXB_DOM_NODE_TYPE_ATTRIBUTE), nnm_p = named_node_map_proto(ctx);
        JSValue nnm = idl_interface_object(ctx, "NamedNodeMap", nnm_p);
        node_install_interface(ctx, global, "Attr", attr_p);
        JS_SetPropertyStr(ctx, (JSValue)global, "NamedNodeMap", nnm);
        JS_FreeValue(ctx, attr_p);
        JS_FreeValue(ctx, nnm_p);
    }
}

void attr_free(JSRuntime *rt)
{
    if (!g_ready) return;
    JS_FreeValueRT(rt, g_nnm_key);   /* the prototypes are the REALMS' — released with their contexts */
    g_nnm_key = JS_UNDEFINED;
    g_ready = 0;
}
