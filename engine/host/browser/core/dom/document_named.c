/* HTML §3.1.7 "DOM tree accessors" — "The Document interface supports named properties" — and the Web IDL §3.9
 * internal methods of a legacy platform object. §3.1.1 "The Document object" declares `[LegacyOverrideBuiltIns]
 * partial interface Document`, so a named property shadows an interface member (`<img name=getElementById>`).
 * Answers come from the running flow's tree in tree order, attributes read at the walk; a [[Get]], [[HasProperty]]
 * or [[GetOwnProperty]] walks as a step machine (dn_step) over the shared resting walk (core/dom/named_walk.h).
 * Named residual: not covered — an index; every `document.x` walks the tree. Next diff builds Blink's named-item
 * map at Lexbor's own tree insertion and removal primitives, which the parser, §8.4.3's document.write feed and
 * the DOM API all call (dom_cow's chokepoints miss the first two), updated as Element::insertedInto/removedFrom
 * update Blink's; the parse runs inside a flow, so the map is per flow's document and rides the flow's delta,
 * never C state sibling flows share. Absence shows as host work per `document.x` growing with the tree. */
#include <string.h>

#include <lexbor/dom/dom.h>
#include <lexbor/ns/ns.h>

#include "check.h"
#include "quickjs.h"
#include "core/dom/attr_list.h"
#include "core/dom/collections.h"
#include "core/dom/document.h"
#include "core/dom/document_named.h"
#include "core/dom/named_walk.h"
#include "core/dom/node.h"
#include "core/html/html_iframe.h"
#include "core/idl_args.h"
#include "core/agent_state.h"
#include "quickjs-step.h"

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

static bool dn_is(JSContext *ctx, lxb_dom_element_t *el, const char *name, size_t n, const void *ud)
{
    (void)ctx; (void)ud;
    return document_named_element_is(el, name, n);
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
    for (lxb_dom_node_t *e = named_walk_successor(doc, doc); e && c < 2; e = named_walk_successor(e, doc))
        if (document_named_element_is(lxb_dom_interface_element(e), name, n)) {
            if (!c) *first = e;
            c++;
        }
    return c;
}

/* §3.1.7's "determine the value of a named property name for a Document", or JS_UNDEFINED where `name` is not a
   supported property name. Owned. */
static JSValue dn_value_of(JSContext *ctx, JSValueConst obj, const char *name, lxb_dom_node_t *first, int c)
{
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

/* ---- the walk as a step machine ------------------------------------------------------------------------- */

#define DN_ALGORITHM "HTML §3.1.7 DOM tree accessors: the Document's named properties"
#define DN_STAGES(X) X(DN_WALK, DN_ALGORITHM)
enum { IDL_STEP_STAGE_BASE(DN_STAGES) DN_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const DN_STEPS[] = { DN_STAGES(JS_STEP_STAGE_LABEL) NULL };
static int g_id_dn_step = -1;

static void dn_walk_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    (void)ctx; (void)st; (void)v;
}

/* f(obj, key, receiver, op), op 0 for [[Get]], 1 for [[HasProperty]] and 2 for [[GetOwnProperty]]: one element
   per entry, then the named property's value (true for op 1), or undefined (false) for a miss. quickjs builds op
   2's descriptor from the class's get_own_property_step_flags, and walks a [[Get]] or [[HasProperty]] miss on to
   the prototype with the operation's receiver. */
static int dn_step(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv, JSValue cb_result,
                   JSValue *presult, JSValue **out_cb, int *out_argc)
{
    NamedWalk *s = st;
    lxb_dom_node_t *doc;
    const char *name;
    size_t n;
    int op;

    (void)out_cb; (void)out_argc;
    JS_FreeValue(ctx, cb_result);
    *presult = JS_UNDEFINED;
    DCHECK(hdr->stage == DN_WALK && argc == 4 && JS_IsString(argv[1]) && JS_VALUE_GET_TAG(argv[3]) == JS_TAG_INT,
           "the Document named-access step was called with a shape the keyed entry does not issue");
    op = JS_VALUE_GET_INT(argv[3]);
    DCHECK(op == 0 || op == 1 || op == 2,
           "the Document named-access step was asked neither [[Get]], [[HasProperty]] nor [[GetOwnProperty]]");
    doc = dn_document(argv[0]);
    name = JS_ToCStringLen(ctx, &n, argv[1]);
    if (!name) return JS_STEP_ABRUPT;
    if (named_walk_next(ctx, s, doc, dn_is, name, n, NULL)) { JS_FreeCString(ctx, name); return JS_STEP_YIELD; }
    if (op == 1) *presult = JS_NewBool(ctx, s->count > 0);
    else *presult = dn_value_of(ctx, argv[0], name, s->first, s->count);
    JS_FreeCString(ctx, name);
    return JS_STEP_DONE;
}

static const IdlStepDecl DN_DECL = { dn_step, sizeof(NamedWalk), dn_walk_visit, NULL, DN_ALGORITHM, DN_STEPS };

/* JSClassExoticMethods.get_own_property_step: the walk for a String key, JS_UNDEFINED for a Symbol. Owned. */
static JSValue dn_get_own_step(JSContext *ctx, JSValueConst obj, JSAtom prop)
{
    JSValue pv = JS_AtomToValue(ctx, prop);
    bool str = JS_IsString(pv);

    (void)obj;
    JS_FreeValue(ctx, pv);
    if (!str) return JS_UNDEFINED;
    DCHECK(g_id_dn_step >= 0, "Document named access asked for its step before document_named_init declared it");
    return idl_step_function(ctx, "Document named access", g_id_dn_step);
}

void document_named_init(JSContext *ctx)
{
    static const IdlArgType ARGS[4] = { IDL_ANY, IDL_ANY, IDL_ANY, IDL_ANY };

    DCHECK(g_id_dn_step < 0, "document_named_init ran twice");
    g_id_dn_step = idl_method_id_step(ctx, ARGS, 4, NULL, 0, &DN_DECL, 0);
    agent_state_id("document", &g_id_dn_step, "HTML §3.1.7 the Document's named-access step declaration");
}

void document_named_free(void)
{
    g_id_dn_step = -1;
    agent_state_reached("document");
}

/* Web IDL §3.9.1 [[GetOwnProperty]] from C. The lookup rests per element as dn_step, which every keyed request
   reaches (get_own_property_step), and [[Set]] ignores named properties (set_ignores_named_properties), so a C
   activation asking for a String key is a caller to route onto the keyed entry. */
static int dn_get_own(JSContext *ctx, JSPropertyDescriptor *desc, JSValueConst obj, JSAtom prop)
{
    const char *name;
    size_t n;

    (void)desc; (void)obj;
    if (!dn_string_name(ctx, prop, &name, &n)) return 0;
    JS_FreeCString(ctx, name);
    DFAIL("a C activation asked a Document's named [[GetOwnProperty]]: the lookup rests per element as dn_step, so "
          "route this caller's read through the keyed entry (get_own_property_step), never a C walk");
    return 0;
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
    for (lxb_dom_node_t *e = named_walk_successor(doc, doc); e; e = named_walk_successor(e, doc)) {
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
        supported = !document_unforgeable_name(document_realm_of(dn_document(obj)), prop) &&
                    dn_count(dn_document(obj), name, n, &first) > 0;
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

/* Named residuals. Not covered: (1) the [[OwnPropertyKeys]], [[DefineOwnProperty]] and [[Delete]] hooks walk the
   tree in C to completion, uncharged; next diff makes each a request the keyed entry answers with dn_step's
   walk; absence shows as such an operation's host time growing with the tree while twDomWalkInput does not.
   (2) A Document
   holding a region of unknown markup (core/dom/unknown_markup.h) answers from the example's tree; next diff forks
   dn_step over the region as window.c's win_named_step does, with markup composed for §3.1.7's element types;
   absence shows as a `document.x` read after a sanitized innerHTML of attacker markup making no @S record. */
static const JSClassExoticMethods DOCUMENT_NAMED_EXOTIC = {
    .get_own_property = dn_get_own,
    .get_own_property_step = dn_get_own_step,
    .get_own_property_step_flags = JS_PROP_ENUMERABLE | JS_PROP_CONFIGURABLE,
    .set_ignores_named_properties = true,
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
