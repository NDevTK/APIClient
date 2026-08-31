/* DOMStringMap — HTML §3.2.2's `dataset`.
 *
 * `el.dataset.userId` is one of the most common things a bundle does with an element, and it was ABSENT — so
 * the read was `undefined`, and undefined does not throw. A page that stores its routing on `data-*` and reads
 * it back took the branch behind the undefined, and the engine reported the surface THAT branch reaches
 * instead of the one the page has. It is the same failure shape innerHTML's missing getter had.
 *
 * IT IS A NAMED PROPERTY GETTER/SETTER/DELETER AND NOTHING ELSE — the IDL is three lines:
 *     interface DOMStringMap {
 *       getter DOMString (DOMString name);
 *       [CEReactions] setter undefined (DOMString name, DOMString value);
 *       [CEReactions] deleter undefined (DOMString name);
 *     };
 * so there is no member list to install; the whole interface is the exotic behaviour. That is why this is its
 * own class rather than something idl_indexed could carry: idl_indexed answers INDICES and has a named getter
 * beside them, and a DOMStringMap has no indices and needs the two write halves as well.
 *
 * THE NAME MANGLING IS THE SPEC, not a convenience. `data-user-id` is `userId`, and the rules are asymmetric in
 * a way that matters: an attribute whose name has an ASCII uppercase after `data-` is NOT exposed at all, and a
 * property name containing `-` followed by a lowercase letter is a SyntaxError rather than a new attribute.
 * Getting those backwards silently exposes or hides half a page's data attributes.
 *
 * WRITES GO THROUGH THE CHOKEPOINT, like every other attribute write — `el.dataset.x = location.hash` is a
 * taint-carrying write and must be per-flow, or a forked arm reads its sibling's value. */
#include <string.h>
#include <stdlib.h>

#include <lexbor/dom/dom.h>

#include "check.h"
#include "quickjs.h"
#include "solver/attr_shadow.h"
#include "solver/concolic.h"
#include "solver/dom_cow.h"
#include "core/idl_args.h"
#include "core/realm.h"
#include "core/dom/element.h"
#include "core/dom/node.h"
#include "core/html/dom_string_map.h"

static JSClassID g_class;
static int       g_ready;

/* The element this map is the dataset OF. The map holds the element's NODE, not a reference to its wrapper:
   the wrapper owns the map (that is what [SameObject] means here), so a map holding the wrapper back would be
   a cycle the collector has to break. A Lexbor node outlives every flow.
   NEVER NULL ON AN OBJECT OF THIS CLASS, and each hook below asserts it rather than this function, so the
   abort carries the SITE: dom_string_map_new is the only thing that builds one and it sets the opaque in the
   same call, and quickjs reaches an exotic hook only through this class's own dispatch — so a NULL here is an
   object of this class that some other route created, and each hook names what IT would answer wrongly. */
static lxb_dom_element_t *dsm_element(JSValueConst obj)
{
    lxb_dom_node_t *n = JS_GetOpaque(obj, g_class);
    return n ? lxb_dom_interface_element(n) : NULL;
}

static bool dsm_is_lower(char c) { return c >= 'a' && c <= 'z'; }
static bool dsm_is_upper(char c) { return c >= 'A' && c <= 'Z'; }

/* §3.2.2 attribute name -> property name: drop `data-`, then each `-` followed by an ASCII lowercase letter
   becomes that letter uppercased. Answers false for an attribute the spec does NOT expose — one that is not
   `data-*` at all, or whose remainder contains an ASCII uppercase (because the reverse mapping could not
   produce it, so exposing it would make dataset non-round-tripping). */
static bool dsm_attr_to_prop(const char *attr, size_t alen, char *out, size_t *outlen)
{
    size_t i, o = 0;

    if (alen < 5 || memcmp(attr, "data-", 5) != 0) return false;
    for (i = 5; i < alen; i++) {
        if (dsm_is_upper(attr[i])) return false;
        if (attr[i] == '-' && i + 1 < alen && dsm_is_lower(attr[i + 1])) {
            out[o++] = (char)(attr[++i] - 32);
            continue;
        }
        out[o++] = attr[i];
    }
    out[o] = 0;
    *outlen = o;
    return true;
}

/* §3.2.2 property name -> attribute name: a `-` followed by an ASCII lowercase letter is a SyntaxError, each
   ASCII uppercase becomes `-` plus its lowercase, and the whole thing is prefixed with `data-`. Returns NULL
   having thrown. The caller frees. */
static char *dsm_prop_to_attr(JSContext *ctx, const char *prop, size_t plen, size_t *outlen)
{
    size_t i, o = 0;
    char *out;

    for (i = 0; i < plen; i++)
        if (prop[i] == '-' && i + 1 < plen && dsm_is_lower(prop[i + 1])) {
            JS_ThrowDOMException(ctx, "SyntaxError",
                                 "a dataset name may not contain a dash followed by a lowercase letter");
            return NULL;
        }
    out = malloc(plen * 2 + 6);
    CHECK(out != NULL, "dataset could not build an attribute name");
    memcpy(out, "data-", 5);
    o = 5;
    for (i = 0; i < plen; i++) {
        if (dsm_is_upper(prop[i])) { out[o++] = '-'; out[o++] = (char)(prop[i] + 32); }
        else out[o++] = prop[i];
    }
    out[o] = 0;
    *outlen = o;
    return out;
}

/* The value of the data-* attribute this property name maps to, or NULL. `pattr` receives the attribute name
   the caller must free when it is non-NULL. */
static const lxb_char_t *dsm_lookup(JSContext *ctx, lxb_dom_element_t *el, JSAtom prop,
                                    char **pattr, size_t *pvlen)
{
    const char *name = JS_AtomToCString(ctx, prop);
    size_t alen = 0;
    char *attr;

    *pattr = NULL;
    if (!name) return NULL;
    attr = dsm_prop_to_attr(ctx, name, strlen(name), &alen);
    JS_FreeCString(ctx, name);
    if (!attr) return NULL;   /* threw: a name the spec refuses */
    *pattr = attr;
    return lxb_dom_element_get_attribute(el, (const lxb_char_t *)attr, alen, pvlen);
}

static int dsm_get_own(JSContext *ctx, JSPropertyDescriptor *desc, JSValueConst obj, JSAtom prop)
{
    lxb_dom_element_t *el = dsm_element(obj);
    const lxb_char_t *v;
    char *attr = NULL;
    size_t vlen = 0;
    JSValue t;

    DCHECK(el != NULL, "a DOMStringMap has no element at its [[GetOwnProperty]] — a `return 0` here is Web IDL "
                       "§3.9.1 [[GetOwnProperty]] reporting P as no supported property name, so every "
                       "`el.dataset.x` would read `undefined` off the prototype chain and a page that routes "
                       "on a data-* attribute would take the branch behind the undefined");
    /* THE TAINT SHADOW ANSWERS FIRST, as it does for getAttribute: a source written here came back out of
       Lexbor as plain bytes with its provenance gone, and a sink reading it would look clean. */
    {
        const char *name = JS_AtomToCString(ctx, prop);
        size_t alen = 0;
        char *a2;
        if (!name) return -1;
        a2 = dsm_prop_to_attr(ctx, name, strlen(name), &alen);
        JS_FreeCString(ctx, name);
        if (!a2) { JS_FreeValue(ctx, JS_GetException(ctx)); return 0; }   /* not a supported name, not a throw */
        t = dom_cow_attr_taint(el, a2);
        if (!JS_IsUndefined(t)) {
            free(a2);
            if (!desc) return 1;
            desc->flags = JS_PROP_ENUMERABLE | JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE;
            desc->value = JS_DupValue(ctx, t);
            desc->getter = desc->setter = JS_UNDEFINED;
            return 1;
        }
        attr = a2;
        v = lxb_dom_element_get_attribute(el, (const lxb_char_t *)attr, alen, &vlen);
    }
    free(attr);
    if (!v) return 0;   /* not a supported property name: the ordinary lookup continues */
    if (!desc) return 1;
    desc->flags = JS_PROP_ENUMERABLE | JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE;
    desc->value = JS_NewStringLen(ctx, (const char *)v, vlen);
    desc->getter = desc->setter = JS_UNDEFINED;
    return 1;
}

static int dsm_has(JSContext *ctx, JSValueConst obj, JSAtom prop)
{
    return dsm_get_own(ctx, NULL, obj, prop);
}

/* §3.2.6.6's SUPPORTED PROPERTY NAMES — what Object.keys(el.dataset) and a for-in see. Every data-* attribute the
   mapping can round-trip, in the order the element holds them. */
static int dsm_own_names(JSContext *ctx, JSPropertyEnum **ptab, uint32_t *plen, JSValueConst obj)
{
    lxb_dom_element_t *el = dsm_element(obj);
    lxb_dom_attr_t *a;
    JSPropertyEnum *tab = NULL;
    uint32_t n = 0, cap = 0;

    *ptab = NULL; *plen = 0;
    DCHECK(el != NULL, "a DOMStringMap has no element at its [[OwnPropertyKeys]] — an empty key list here is "
                       "§3.9.6's positive statement that this object supports no property names, so "
                       "`Object.keys(el.dataset)` and every for-in over it would report an element's data-* "
                       "attributes as absent rather than crashing");
    for (a = lxb_dom_element_first_attribute(el); a; a = lxb_dom_element_next_attribute(a)) {
        size_t alen = 0, plen2 = 0;
        const lxb_char_t *an = lxb_dom_attr_qualified_name(a, &alen);
        char *buf;
        if (!an) continue;
        buf = malloc(alen + 1);
        CHECK(buf != NULL, "dataset could not build a property name");
        if (!dsm_attr_to_prop((const char *)an, alen, buf, &plen2)) { free(buf); continue; }
        if (n == cap) {
            uint32_t c = cap ? cap * 2 : 8;
            JSPropertyEnum *t = js_realloc(ctx, tab, sizeof(*t) * c);
            if (!t) { free(buf); js_free(ctx, tab); return -1; }
            tab = t; cap = c;
        }
        tab[n].is_enumerable = 1;
        tab[n].atom = JS_NewAtomLen(ctx, buf, plen2);
        free(buf);
        if (tab[n].atom == JS_ATOM_NULL) { while (n--) JS_FreeAtom(ctx, tab[n].atom); js_free(ctx, tab); return -1; }
        n++;
    }
    *ptab = tab;
    *plen = n;
    return 0;
}

/* §3.2.2's SETTER. It is define_own_property rather than set_property because that is where an assignment to a
   property this object does not already have lands, and it is where one it DOES have lands too once
   get_own_property has reported it writable. */
static int dsm_define_own(JSContext *ctx, JSValueConst obj, JSAtom prop, JSValueConst val,
                          JSValueConst getter, JSValueConst setter, int flags)
{
    lxb_dom_element_t *el = dsm_element(obj);
    const char *name;
    char *attr;
    size_t alen = 0;

    (void)flags;
    DCHECK(el != NULL, "a DOMStringMap has no element at its [[DefineOwnProperty]] — a `return 0` here is Web "
                       "IDL §3.9.3's \"return false\", which reaches a page as a silently dropped `el.dataset.x "
                       "= v` in sloppy mode and a TypeError in strict, so the write would be REFUSED rather "
                       "than the broken object named");
    /* An ACCESSOR cannot be defined on a DOMStringMap — the IDL declares a value setter and nothing else. */
    if (!JS_IsUndefined(getter) || !JS_IsUndefined(setter)) {
        JS_ThrowTypeError(ctx, "dataset properties are values, not accessors");
        return -1;
    }
    name = JS_AtomToCString(ctx, prop);
    if (!name) return -1;
    attr = dsm_prop_to_attr(ctx, name, strlen(name), &alen);
    JS_FreeCString(ctx, name);
    if (!attr) return -1;   /* the SyntaxError §3.2.2 states */

    /* A CONCOLIC value has no bytes to store, exactly as in setAttribute: record it in the shadow so the read
       gives the SAME concolic back, and write its shape into the tree so a serialisation still shows something. */
    if (concolic_is(val)) {
        const char *shape = concolic_shape_c(val);
        dom_cow_set_attribute(el, attr, shape ? shape : "", shape ? strlen(shape) : 0, val);
        free(attr);
        return 1;
    }
    {
        const char *s = JS_ToCString(ctx, val);
        if (!s) { free(attr); return -1; }
        /* JS_UNDEFINED clears any old taint: a concrete write says this attribute is no longer a source. */
        dom_cow_set_attribute(el, attr, s, strlen(s), JS_UNDEFINED);   /* chokepoint: capture-then-mutate, per flow */
        JS_FreeCString(ctx, s);
    }
    free(attr);
    return 1;
}

/* §3.2.2's DELETER. `delete el.dataset.x` removes the attribute; a name that does not map is simply not there,
   which [[Delete]] reports as success. */
static int dsm_delete(JSContext *ctx, JSValueConst obj, JSAtom prop)
{
    lxb_dom_element_t *el = dsm_element(obj);
    char *attr = NULL;
    size_t vlen = 0;
    const lxb_char_t *v;

    DCHECK(el != NULL, "a DOMStringMap has no element at its [[Delete]] — a `true` here is Web IDL §3.9.4 "
                       "[[Delete]]'s final step, reached when the object has no own property with that name, "
                       "so `delete el.dataset.x` would report success having removed nothing and the attribute "
                       "would still be there on the next read");
    v = dsm_lookup(ctx, el, prop, &attr, &vlen);
    if (!attr) { JS_FreeValue(ctx, JS_GetException(ctx)); return true; }
    if (v) dom_cow_remove_attribute(el, attr);   /* the removal chokepoint, so it reverts per flow */
    free(attr);
    return true;
}

/* WEB IDL §3.9.5 [[PreventExtensions]]: "When the [[PreventExtensions]] internal method of a legacy platform
 * object is called, the following steps are taken: Return false." Its note says why in one sentence — "this
 * keeps legacy platform objects extensible by making [[PreventExtensions]] fail for them."
 *
 * A DOMStringMap IS one, and the IDL at the head of this file is the whole argument: a named property getter,
 * setter and deleter, and no [Global]. The definition is Web IDL §2.12 Objects implementing interfaces' and
 * not §3.9's — "Legacy platform objects are platform objects that implement an interface which does not have a
 * [Global] extended attribute, and which supports indexed properties, named properties, or both" — so the
 * number that says WHY this class qualifies is not the number that says what it must answer.
 *
 * It is the third answer this object owes to one question, beside §3.9.3's refusal to define a shadowing own
 * property and §3.9.4's delete: this map's property set is the ELEMENT'S ATTRIBUTES and not the page's, and a
 * successful freeze would have said the opposite about the same object while `setAttribute` went on changing
 * it underneath.
 *
 * IT ASKS NOTHING ABOUT THE OBJECT — every object of this class is a dataset, so there is no per-object
 * question here the way there is for a CSSRule, which is a legacy platform object only when it is the
 * `@keyframes` rule. The element is not read, which is also why this is the one hook in the table with no
 * DCHECK on it: it has nothing to be wrong about. */
static int dsm_prevent_extensions(JSContext *ctx, JSValueConst obj)
{
    (void)ctx; (void)obj;
    return 0;
}

static JSClassExoticMethods g_exotic = {
    .get_own_property = dsm_get_own,
    .get_own_property_names = dsm_own_names,
    .delete_property = dsm_delete,
    .define_own_property = dsm_define_own,
    .has_property = dsm_has,
    .prevent_extensions = dsm_prevent_extensions,
    /* The lookup is a name mangle and a read of the element's own attributes. There is no accessor in a Web IDL
       named property getter by construction, which is what lets the engine's accessor walk run it from C. */
    .get_own_property_no_user_code = true,
};

static JSClassDef g_class_def = { "DOMStringMap", NULL, NULL, NULL, &g_exotic };

void dom_string_map_init(JSContext *ctx)
{
    DCHECK(!g_ready, "dom_string_map_init ran twice — one instance is one document");
    g_class = 0;
    JS_NewClassID(JS_GetRuntime(ctx), &g_class);
    CHECK(JS_NewClass(JS_GetRuntime(ctx), g_class, &g_class_def) == 0,
          "the DOMStringMap class could not be registered");
    g_ready = 1;
    realm_declare_intrinsic(dom_string_map_install_proto);
}

/* §3.2.6.6's INTERFACE PROTOTYPE OBJECT, FOR ONE REALM. */
void dom_string_map_install_proto(JSContext *ctx)
{
    JSValue proto, prev;

    DCHECK(g_ready, "a realm asked for DOMStringMap.prototype before the class was declared");
    prev = JS_GetClassProto(ctx, g_class);
    DCHECK(JS_IsNull(prev), "dom_string_map_install_proto ran twice in one realm");
    JS_FreeValue(ctx, prev);

    proto = JS_NewObject(ctx);
    CHECK(!JS_IsException(proto), "DOMStringMap.prototype could not be allocated");
    idl_interface_tag(ctx, proto, "DOMStringMap");
    JS_SetClassProto(ctx, g_class, proto);
}

JSValue dom_string_map_new(JSContext *ctx, lxb_dom_element_t *el)
{
    JSValue obj;

    DCHECK(g_ready, "a dataset was asked for before dom_string_map_init ran");
    {
        JSValue proto = JS_GetClassProto(ctx, g_class);
        DCHECK(!JS_IsNull(proto), "a dataset was minted in a realm that never ran its install");
        obj = JS_NewObjectProtoClass(ctx, proto, g_class);
        JS_FreeValue(ctx, proto);
    }
    if (JS_IsException(obj)) return obj;
    JS_SetOpaque(obj, lxb_dom_interface_node(el));
    return obj;
}

void dom_string_map_install(JSContext *ctx, JSValueConst global)
{
    JSValue ctor;

    DCHECK(g_ready, "the DOMStringMap interface was installed before its prototype was built");
    {
        JSValue proto = JS_GetClassProto(ctx, g_class);
        DCHECK(!JS_IsNull(proto), "DOMStringMap was installed into a realm that never ran its proto build");
        ctor = idl_interface_object(ctx, "DOMStringMap", proto);
        JS_FreeValue(ctx, proto);
    }
    JS_SetPropertyStr(ctx, (JSValue)global, "DOMStringMap", ctor);
}

void dom_string_map_free(JSRuntime *rt)
{
    if (!g_ready) return;
    g_ready = 0;   /* the prototypes are the REALMS' — released with their contexts */
}
