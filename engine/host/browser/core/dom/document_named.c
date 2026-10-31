/* HTML §3.1.7 "DOM tree accessors" — "The Document interface supports named properties" — and the Web IDL §3.9
 * internal methods that make a Document a legacy platform object. HTML §3.1.1 "The Document object" declares
 * `[LegacyOverrideBuiltIns] partial interface Document`, so a named property shadows the interface's own members
 * (`<img name=getElementById>` makes `document.getElementById` that element); the named property visibility
 * algorithm (§3.9.7) decides it, and step 3 answers true for every supported name the object has no own property
 * for.
 *
 * Every question is answered from the document tree as the running flow sees it, by a walk in tree order, with
 * the content attributes read at the walk. Named residual: not covered — an index; each named lookup on a
 * Document costs a walk of its tree, and quickjs reaches get_own_property after every own-property miss, so
 * nearly every `document.x` pays it. Next diff builds Blink's named-item map, maintained at dom_cow's insert,
 * remove and attribute chokepoints, once every parse path (html_parse.c's tree construction and §8.4.3's
 * document.write feed) reaches them; an index fed only by the chokepoints would miss parsed elements. Absence
 * shows as host work per `document.x` growing with the document's element count. */
#include <string.h>

#include <lexbor/dom/dom.h>
#include <lexbor/ns/ns.h>

#include "check.h"
#include "quickjs.h"
#include "core/dom/attr_list.h"
#include "core/dom/collections.h"
#include "core/dom/document_named.h"
#include "core/dom/node.h"
#include "core/html/html_iframe.h"

enum { DN_NONE = 0, DN_EMBED, DN_FORM, DN_IFRAME, DN_IMG, DN_OBJECT };

/* Which of §3.1.7's five element types `el` is: an HTML element by its local name. */
static int dn_type(lxb_dom_element_t *el)
{
    static const struct { const char *name; size_t len; int type; } T[] = {
        { "embed", 5, DN_EMBED }, { "form", 4, DN_FORM }, { "iframe", 6, DN_IFRAME },
        { "img", 3, DN_IMG }, { "object", 6, DN_OBJECT },
    };
    size_t len = 0;
    const lxb_char_t *l;

    if (lxb_dom_interface_node(el)->ns != LXB_NS_HTML) return DN_NONE;
    l = lxb_dom_element_local_name(el, &len);
    for (size_t i = 0; l && i < sizeof T / sizeof T[0]; i++)
        if (len == T[i].len && memcmp(l, T[i].name, len) == 0) return T[i].type;
    return DN_NONE;
}

/* The value of `el`'s (null namespace, `local`) content attribute, `*len` bytes; NULL where absent or valueless. */
static const lxb_char_t *dn_attr(lxb_dom_element_t *el, const char *local, size_t *len)
{
    lxb_dom_attr_t *a = dom_attr_get_ns(el, NULL, local);

    *len = 0;
    return a ? lxb_dom_attr_value(a, len) : NULL;
}

static bool dn_eq(const lxb_char_t *v, size_t vl, const char *name, size_t n)
{
    return v && vl == n && memcmp(v, name, n) == 0;
}

bool document_named_element_is(lxb_dom_element_t *el, const char *name, size_t n)
{
    size_t nl = 0, il = 0;
    const lxb_char_t *nv, *iv;
    int t;

    if (!n) return false;                       /* "if name is not the empty string" */
    t = dn_type(el);
    if (t == DN_NONE) return false;
    nv = dn_attr(el, "name", &nl);
    if (dn_eq(nv, nl, name, n)) return true;    /* embed, form, iframe, img or object, by its name attribute */
    if (t != DN_OBJECT && t != DN_IMG) return false;
    iv = dn_attr(el, "id", &il);
    if (!dn_eq(iv, il, name, n)) return false;
    return t == DN_OBJECT || (nv && nl > 0);    /* an object by its id; an img by its id with a non-empty name */
}

/* The next element after `n` in tree order within the document tree rooted at `root`. */
static lxb_dom_node_t *dn_next(lxb_dom_node_t *n, lxb_dom_node_t *root)
{
    do {
        if (n->first_child) { n = n->first_child; continue; }
        while (n != root && !n->next) n = n->parent;
        if (n == root) return NULL;
        n = n->next;
    } while (n->type != LXB_DOM_NODE_TYPE_ELEMENT);
    return n;
}

/* The Document node a Document wrapper stands for. */
static lxb_dom_node_t *dn_document(JSValueConst obj)
{
    lxb_dom_node_t *d = node_of(obj);

    DCHECK(d != NULL && d->type == LXB_DOM_NODE_TYPE_DOCUMENT,
           "a Document named-property hook ran on an object that is no Document wrapper — the hooks are reached "
           "only through the Document classes' own dispatch");
    return d;
}

/* §3.1.7's "list of named elements with the name name that are in a document tree with the Document as their
   root", counted to two: the first in `*first`. */
static int dn_count(lxb_dom_node_t *doc, const char *name, size_t n, lxb_dom_node_t **first)
{
    int c = 0;

    *first = NULL;
    for (lxb_dom_node_t *e = dn_next(doc, doc); e && c < 2; e = dn_next(e, doc))
        if (document_named_element_is(lxb_dom_interface_element(e), name, n)) {
            if (!c) *first = e;
            c++;
        }
    return c;
}

/* §3.1.7's "determine the value of a named property name for a Document", or JS_UNDEFINED where `name` is not a
   supported property name. Owned. */
static JSValue dn_value(JSContext *ctx, JSValueConst obj, const char *name, size_t n)
{
    lxb_dom_node_t *first;
    int c = dn_count(dn_document(obj), name, n, &first);
    JSValue wrap, nav;

    if (c == 0) return JS_UNDEFINED;
    if (c > 1) return collections_document_named(ctx, obj, name);                      /* step 4 */
    wrap = node_wrap(ctx, first);
    /* Step 2: one iframe with a content navigable is that navigable's active WindowProxy, asked of the
       component that owns the slot rather than through `contentWindow`, an IDL accessor no C walk may run. */
    if (dn_type(lxb_dom_interface_element(first)) == DN_IFRAME) {
        nav = iframe_navigable(ctx, wrap);
        if (JS_IsObject(nav)) { JS_FreeValue(ctx, wrap); return nav; }
        JS_FreeValue(ctx, nav);
    }
    return wrap;                                                                         /* step 3 */
}

/* `prop` as a String property name in `*name`/`*n` (JS_FreeCString it); false for a Symbol, which names no named
   property. */
static bool dn_string_name(JSContext *ctx, JSAtom prop, const char **name, size_t *n)
{
    JSValue pv = JS_AtomToValue(ctx, prop);
    bool str = JS_IsString(pv);

    JS_FreeValue(ctx, pv);
    if (!str) return false;
    *name = JS_AtomToCStringLen(ctx, n, prop);
    CHECK(*name != NULL, "document: OOM reading a property name");
    return true;
}

/* Web IDL §3.9.1 [[GetOwnProperty]], reached by quickjs after the object's own properties missed, which is the
   visibility algorithm's step 2; the Document's [LegacyOverrideBuiltIns] is its step 3. LegacyPlatformObject-
   GetOwnProperty's descriptor: no named property setter, so not writable; enumerable and configurable. */
static int dn_get_own(JSContext *ctx, JSPropertyDescriptor *desc, JSValueConst obj, JSAtom prop)
{
    const char *name;
    size_t n;
    JSValue v;

    if (!dn_string_name(ctx, prop, &name, &n)) return 0;
    v = dn_value(ctx, obj, name, n);
    JS_FreeCString(ctx, name);
    if (JS_IsUndefined(v)) return 0;
    if (!desc) { JS_FreeValue(ctx, v); return 1; }
    desc->flags = JS_PROP_ENUMERABLE | JS_PROP_CONFIGURABLE;
    desc->getter = JS_UNDEFINED;
    desc->setter = JS_UNDEFINED;
    desc->value = v;
    return 1;
}

/* Whether `obj` has an ordinary own property `prop`: the visibility algorithm's step 2, asked without the hooks. */
static bool dn_has_own(JSContext *ctx, JSValueConst obj, JSAtom prop)
{
    JSPropertyDescriptor d;
    int r = JS_GetOwnSlotDesc(ctx, &d, obj, prop);

    CHECK(r >= 0, "document: an own property could not be read back");
    if (r > 0) { JS_FreeValue(ctx, d.value); JS_FreeValue(ctx, d.getter); JS_FreeValue(ctx, d.setter); }
    return r > 0;
}

/* Web IDL §3.9.6 [[OwnPropertyKeys]] step 3: the supported property names that are visible — §3.1.7's, "for each
   element element in a document tree with document as its root, in tree order, ignoring later duplicates", the
   id content attribute's value and then the name content attribute's, each where the element is a named element
   with that value as its name.
   Named residual: not covered — §3.9.6's order; quickjs appends an exotic object's names after all its own keys,
   where §3.9.6 puts them first. Next diff builds the exotic names' place in JS_GetOwnPropertyNames for a legacy
   platform object. Absence shows as `Reflect.ownKeys(document)` listing `location` before the named elements. */
static int dn_own_names(JSContext *ctx, JSPropertyEnum **ptab, uint32_t *plen, JSValueConst obj)
{
    lxb_dom_node_t *doc = dn_document(obj);
    JSPropertyEnum *tab = NULL;
    uint32_t n = 0, cap = 0;

    *ptab = NULL;
    *plen = 0;
    for (lxb_dom_node_t *e = dn_next(doc, doc); e; e = dn_next(e, doc)) {
        lxb_dom_element_t *el = lxb_dom_interface_element(e);
        static const char *const LOCAL[2] = { "id", "name" };

        for (int k = 0; k < 2; k++) {
            size_t vl = 0;
            const lxb_char_t *v = dn_attr(el, LOCAL[k], &vl);
            JSAtom a;
            uint32_t j;

            if (!v || !document_named_element_is(el, (const char *)v, vl)) continue;
            a = JS_NewAtomLen(ctx, (const char *)v, vl);
            CHECK(a != JS_ATOM_NULL, "document: OOM naming a supported property name");
            for (j = 0; j < n && tab[j].atom != a; j++) { }
            if (j < n || dn_has_own(ctx, obj, a)) { JS_FreeAtom(ctx, a); continue; }
            if (n == cap) {
                JSPropertyEnum *t = js_realloc(ctx, tab, sizeof *t * (cap = cap ? cap * 2 : 8));
                CHECK(t != NULL, "document: OOM listing supported property names");
                tab = t;
            }
            tab[n].is_enumerable = 1;
            tab[n].atom = a;
            n++;
        }
    }
    *ptab = tab;
    *plen = n;
    return 0;
}

/* HTML §3.1.1's one unforgeable member: `[PutForwards=href, LegacyUnforgeable] readonly attribute Location?
   location`. */
static bool dn_unforgeable(JSContext *ctx, JSAtom prop)
{
    const char *s = JS_AtomToCString(ctx, prop);
    bool u = s && strcmp(s, "location") == 0;

    JS_FreeCString(ctx, s);
    return u;
}

/* Web IDL §3.9.3 [[DefineOwnProperty]] step 2: a String that is a supported property name and not unforgeable,
   on an object with [LegacyOverrideBuiltIns] and no named property setter, is refused (step 2.2.1); anything else
   is OrdinaryDefineOwnProperty (step 3). */
static int dn_define_own(JSContext *ctx, JSValueConst obj, JSAtom prop, JSValueConst val, JSValueConst getter,
                         JSValueConst setter, int flags)
{
    const char *name;
    size_t n;
    lxb_dom_node_t *first;
    bool supported = false;

    if (dn_string_name(ctx, prop, &name, &n)) {
        supported = !dn_unforgeable(ctx, prop) && dn_count(dn_document(obj), name, n, &first) > 0;
        JS_FreeCString(ctx, name);
    }
    if (supported)
        return JS_RefuseOrThrowTypeError(ctx, flags, "a Document's named property has no setter and overrides "
                                                     "its built-ins, so it cannot be redefined");
    return JS_DefineProperty(ctx, obj, prop, val, getter, setter, flags | JS_PROP_NO_EXOTIC);
}

/* Web IDL §3.9.4 [[Delete]] step 2: a visible named property, with no named property deleter, is not deleted;
   quickjs reaches this only after the own properties missed, so anything else is step 4's true. */
static int dn_delete(JSContext *ctx, JSValueConst obj, JSAtom prop)
{
    const char *name;
    size_t n;
    lxb_dom_node_t *first;
    int c = 0;

    if (!dn_string_name(ctx, prop, &name, &n)) return 1;
    c = dn_count(dn_document(obj), name, n, &first);
    JS_FreeCString(ctx, name);
    return c == 0;
}

/* Web IDL §3.9.5 [[PreventExtensions]]: "Return false." */
static int dn_prevent_extensions(JSContext *ctx, JSValueConst obj)
{
    (void)ctx; (void)obj;
    return 0;
}

/* Named residual: not covered — a Document holding a region of unknown markup (core/dom/unknown_markup.h)
   answers from the example's tree; Window's named access forks over the region (window.c's
   get_own_property_step). Next diff builds the same step for §3.1.7, with markup composed for the Document's
   named element types. Absence shows as a `document.x` read after a sanitized innerHTML of attacker markup
   making no @S record. */
static const JSClassExoticMethods DOCUMENT_NAMED_EXOTIC = {
    .get_own_property = dn_get_own,
    .get_own_property_names = dn_own_names,
    .define_own_property = dn_define_own,
    .delete_property = dn_delete,
    .prevent_extensions = dn_prevent_extensions,
    /* A tree walk, content attribute reads and the iframe component's slot: no page code. */
    .get_own_property_no_user_code = true,
};

const JSClassExoticMethods *document_named_exotic(void)
{
    return &DOCUMENT_NAMED_EXOTIC;
}
