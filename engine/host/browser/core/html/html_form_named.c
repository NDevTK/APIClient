/* HTML §4.10.3 The form element: "[Exposed=Window, LegacyOverrideBuiltIns, LegacyUnenumerableNamedProperties]
 * interface HTMLFormElement" declares `getter (RadioNodeList or Element) (DOMString name)`, so a form element is
 * a Web IDL §3.9 legacy platform object, and a named property shadows an interface member (`<input name=action>`
 * answers `form.action`). The value is HTML §4.10.3's "determine the value of a named property": the listed
 * elements whose form owner is the form, with an `id` or `name` equal to the name and no `input` in the Image
 * Button state; failing those, the `img` elements so owned; failing those, the past names map. A [[Get]],
 * [[HasProperty]] or [[GetOwnProperty]] walks as a step machine (fn_step) over the shared resting walk
 * (core/dom/named_walk.h), asking each element whose attribute matches for its form owner (html_form_owner_of). */
#include <string.h>

#include <lexbor/dom/dom.h>
#include <lexbor/ns/ns.h>

#include "check.h"
#include "quickjs.h"
#include "quickjs-step.h"
#include "core/agent_state.h"
#include "core/idl_args.h"
#include "core/dom/attr_list.h"
#include "core/dom/named_walk.h"
#include "core/dom/node.h"
#include "core/html/html_form.h"
#include "core/html/html_form_named.h"

static JSClassID g_form_class;
static int g_id_fn_step = -1;
/* The past names map's slot key on a form's wrapper: a Symbol, so a page can neither see nor write the map. */
static JSValue g_past_key = JS_UNDEFINED;
static JSAtom g_atom_past = JS_ATOM_NULL;

JSClassID html_form_named_class(void)
{
    return g_form_class;
}

/* The form element a form wrapper stands for. */
static lxb_dom_node_t *fn_form(JSValueConst obj)
{
    lxb_dom_node_t *f = node_of(obj);

    DCHECK(f != NULL && f->type == LXB_DOM_NODE_TYPE_ELEMENT,
           "a form named-property hook ran on an object that is no form element's wrapper — the hooks are reached "
           "only through the form class's own dispatch");
    return f;
}

static bool fn_attr_is(lxb_dom_element_t *el, const char *local, const char *name, size_t n)
{
    lxb_dom_attr_t *a = dom_attr_get_ns(el, NULL, local);
    size_t vl = 0;
    const lxb_char_t *v = a ? lxb_dom_attr_value(a, &vl) : NULL;

    return v && vl == n && memcmp(v, name, n) == 0;
}

/* "have either an id attribute or a name attribute equal to name". */
static bool fn_named(lxb_dom_element_t *el, const char *name, size_t n)
{
    return fn_attr_is(el, "id", name, n) || fn_attr_is(el, "name", name, n);
}

/* Whether `wrap`'s form owner is `form`. */
static bool fn_owned_by(JSContext *ctx, JSValueConst wrap, JSValueConst form)
{
    JSValue owner = html_form_owner_of(ctx, wrap);
    bool mine = JS_IsObject(owner) && JS_VALUE_GET_PTR(owner) == JS_VALUE_GET_PTR(form);

    JS_FreeValue(ctx, owner);
    return mine;
}

/* Step 1's candidates: a listed element, no Image Button, named `name`, owned by the form `*ud`. The attribute is
   asked first, so an element that cannot match mints no wrapper. */
static bool fn_listed_is(JSContext *ctx, lxb_dom_element_t *el, const char *name, size_t n, const void *ud)
{
    lxb_dom_node_t *node = lxb_dom_interface_node(el);
    JSValue wrap;
    bool hit;

    if (!html_form_maybe_associated(node) || !fn_named(el, name, n)) return false;
    wrap = node_wrap(ctx, node);
    hit = html_form_is_named_listed(ctx, wrap) && fn_owned_by(ctx, wrap, *(const JSValueConst *)ud);
    JS_FreeValue(ctx, wrap);
    return hit;
}

/* Step 2's candidates: an `img` named `name`, owned by the form `*ud`. */
static bool fn_img_is(JSContext *ctx, lxb_dom_element_t *el, const char *name, size_t n, const void *ud)
{
    lxb_dom_node_t *node = lxb_dom_interface_node(el);
    size_t len = 0;
    const lxb_char_t *l;
    JSValue wrap;
    bool hit;

    if (node->ns != LXB_NS_HTML) return false;
    l = lxb_dom_element_local_name(el, &len);
    if (!l || len != 3 || memcmp(l, "img", 3) != 0 || !fn_named(el, name, n)) return false;
    wrap = node_wrap(ctx, node);
    hit = fn_owned_by(ctx, wrap, *(const JSValueConst *)ud);
    JS_FreeValue(ctx, wrap);
    return hit;
}

/* The form's past names map, an own slot on its wrapper holding a null-prototype object from name to element, so
   it forks with the flow that wrote it. Owned; JS_UNDEFINED where the form has none and `create` is false. */
static JSValue fn_past_map(JSContext *ctx, JSValueConst form, bool create)
{
    JSValue m;

    DCHECK(g_atom_past != JS_ATOM_NULL, "a form's past names map was asked for before its slot key was minted");
    if (JS_GetOwnSlot(ctx, &m, form, g_atom_past) > 0) return m;
    if (!create) return JS_UNDEFINED;
    m = JS_NewObjectProto(ctx, JS_NULL);
    CHECK(!JS_IsException(m), "forms: OOM making a form's past names map");
    CHECK(JS_DefinePropertyValue(ctx, (JSValue)form, g_atom_past, JS_DupValue(ctx, m),
                                 JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE) >= 0,
          "forms: a form's wrapper refused its past names map");
    return m;
}

/* The element the past names map holds for `key`, owned, or JS_UNDEFINED. */
static JSValue fn_past_get(JSContext *ctx, JSValueConst form, JSValueConst key)
{
    JSValue m = fn_past_map(ctx, form, false), v = JS_UNDEFINED;
    JSAtom a;

    if (JS_IsUndefined(m)) return JS_UNDEFINED;
    a = JS_ValueToAtom(ctx, key);
    CHECK(a != JS_ATOM_NULL, "forms: OOM naming a past names map entry");
    if (JS_GetOwnSlot(ctx, &v, m, a) <= 0) v = JS_UNDEFINED;
    JS_FreeAtom(ctx, a);
    JS_FreeValue(ctx, m);
    return v;
}

/* Step 5: "Add a mapping from name to the node in candidates in the form element's past names map, replacing the
   previous entry with the same name, if any." */
static void fn_past_set(JSContext *ctx, JSValueConst form, JSValueConst key, JSValueConst el)
{
    JSValue m = fn_past_map(ctx, form, true);
    JSAtom a = JS_ValueToAtom(ctx, key);

    CHECK(a != JS_ATOM_NULL, "forms: OOM naming a past names map entry");
    CHECK(JS_DefinePropertyValue(ctx, m, a, JS_DupValue(ctx, el), JS_PROP_C_W_E) >= 0,
          "forms: a past names map refused an entry");
    JS_FreeAtom(ctx, a);
    JS_FreeValue(ctx, m);
}

/* ---- the lookup as a step machine ----------------------------------------------------------------------- */

#define FN_ALGORITHM "HTML §4.10.3 The form element: determine the value of a named property"
#define FN_STAGES(X) X(FN_WALK, FN_ALGORITHM)
enum { IDL_STEP_STAGE_BASE(FN_STAGES) FN_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const FN_STEPS[] = { FN_STAGES(JS_STEP_STAGE_LABEL) NULL };

/* Which candidates the walk is over: step 1's listed elements, then step 2's `img` elements. */
enum { FN_LISTED = 0, FN_IMG };

typedef struct {
    NamedWalk walk;
    int phase;
} FormNamedState;

static void fn_walk_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    (void)ctx; (void)st; (void)v;
}

/* f(obj, key, receiver, op), op 0 for [[Get]], 1 for [[HasProperty]] and 2 for [[GetOwnProperty]]: one element
   per entry, then the named property's value (true for op 1), or undefined (false) for a miss, which quickjs
   walks on to the prototype. The empty string is no supported property name, so it misses. */
static int fn_step(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv, JSValue cb_result,
                   JSValue *presult, JSValue **out_cb, int *out_argc)
{
    FormNamedState *s = st;
    lxb_dom_node_t *root;
    const char *name;
    size_t n;
    int op;
    JSValue v;

    (void)out_cb; (void)out_argc;
    JS_FreeValue(ctx, cb_result);
    *presult = JS_UNDEFINED;
    DCHECK(hdr->stage == FN_WALK && argc == 4 && JS_IsString(argv[1]) && JS_VALUE_GET_TAG(argv[3]) == JS_TAG_INT,
           "the form named-access step was called with a shape the keyed entry does not issue");
    op = JS_VALUE_GET_INT(argv[3]);
    DCHECK(op == 0 || op == 1 || op == 2,
           "the form named-access step was asked neither [[Get]], [[HasProperty]] nor [[GetOwnProperty]]");
    root = node_root(fn_form(argv[0]));
    name = JS_ToCStringLen(ctx, &n, argv[1]);
    if (!name) return JS_STEP_ABRUPT;
    if (n == 0) { JS_FreeCString(ctx, name); *presult = op == 1 ? JS_FALSE : JS_UNDEFINED; return JS_STEP_DONE; }
    if (named_walk_next(ctx, &s->walk, root, s->phase == FN_LISTED ? fn_listed_is : fn_img_is, name, n, &argv[0])) {
        JS_FreeCString(ctx, name);
        return JS_STEP_YIELD;
    }
    JS_FreeCString(ctx, name);
    if (s->walk.count == 0 && s->phase == FN_LISTED) {   /* step 2: no listed candidate, so the `img` ones */
        s->phase = FN_IMG;
        memset(&s->walk, 0, sizeof s->walk);
        return JS_STEP_YIELD;
    }
    if (s->walk.count == 0) {                            /* step 3: the past names map, or a miss */
        v = fn_past_get(ctx, argv[0], argv[1]);
        if (op == 1) { *presult = JS_NewBool(ctx, !JS_IsUndefined(v)); JS_FreeValue(ctx, v); }
        else *presult = v;
        return JS_STEP_DONE;
    }
    if (op == 1) { *presult = JS_TRUE; return JS_STEP_DONE; }
    /* Step 4. Named residual. Not covered: two or more candidates, whose value is a live RadioNodeList. Next diff
       builds RadioNodeList (DOM §4.2.10.1's NodeList with §4.10.18.4's value) over this file's filter. Absence
       shows as this DFAIL. */
    if (s->walk.count > 1)
        DFAIL("a form's named property has two or more candidates, whose value is a live RadioNodeList — build "
              "RadioNodeList (DOM §4.2.10.1's NodeList with §4.10.18.4's value) over this file's candidate filter");
    v = node_wrap(ctx, s->walk.first);                   /* steps 5-6, and the release arm of step 4 */
    fn_past_set(ctx, argv[0], argv[1], v);
    *presult = v;
    return JS_STEP_DONE;
}

static const IdlStepDecl FN_DECL = { fn_step, sizeof(FormNamedState), fn_walk_visit, NULL, FN_ALGORITHM, FN_STEPS };

/* JSClassExoticMethods.get_own_property_step: the walk for a String key, JS_UNDEFINED for a Symbol. Owned. */
static JSValue fn_get_own_step(JSContext *ctx, JSValueConst obj, JSAtom prop)
{
    JSValue pv = JS_AtomToValue(ctx, prop);
    bool str = JS_IsString(pv);

    (void)obj;
    JS_FreeValue(ctx, pv);
    if (!str) return JS_UNDEFINED;
    DCHECK(g_id_fn_step >= 0, "form named access asked for its step before html_form_named_declare declared it");
    return idl_step_function(ctx, "HTMLFormElement named access", g_id_fn_step);
}

/* `prop` as a String property name in `*name`/`*n` (JS_FreeCString it); false for a Symbol. */
static bool fn_string_name(JSContext *ctx, JSAtom prop, const char **name, size_t *n)
{
    JSValue pv = JS_AtomToValue(ctx, prop);
    bool str = JS_IsString(pv);

    JS_FreeValue(ctx, pv);
    if (!str) return false;
    *name = JS_AtomToCStringLen(ctx, n, prop);
    CHECK(*name != NULL, "forms: OOM reading a property name");
    return true;
}

/* Web IDL §3.9.1 [[GetOwnProperty]] from C. The lookup rests per element as fn_step, which every keyed request
   reaches (get_own_property_step), and [[Set]] ignores named properties (set_ignores_named_properties), so a C
   activation asking for a String key is a caller to route onto the keyed entry. */
static int fn_get_own(JSContext *ctx, JSPropertyDescriptor *desc, JSValueConst obj, JSAtom prop)
{
    const char *name;
    size_t n;

    (void)desc; (void)obj;
    if (!fn_string_name(ctx, prop, &name, &n)) return 0;
    JS_FreeCString(ctx, name);
    DFAIL("a C activation asked a form's named [[GetOwnProperty]]: the lookup rests per element as fn_step, so "
          "route this caller's read through the keyed entry (get_own_property_step), never a C walk");
    return 0;
}

/* Whether `name` is a supported property name of the form `obj`: a candidate of either step, or a past name.
   Walked in C to completion, uncharged: the Document's named residual (1) in core/dom/document_named.c, which
   the same next diff retires for both. */
static bool fn_supported(JSContext *ctx, JSValueConst obj, JSAtom prop)
{
    lxb_dom_node_t *root = node_root(fn_form(obj));
    const char *name;
    size_t n;
    bool hit = false;
    JSValue key, past;

    if (!fn_string_name(ctx, prop, &name, &n)) return false;
    if (n == 0) { JS_FreeCString(ctx, name); return false; }
    for (lxb_dom_node_t *e = named_walk_successor(root, root); e && !hit; e = named_walk_successor(e, root))
        hit = fn_listed_is(ctx, lxb_dom_interface_element(e), name, n, &obj) ||
              fn_img_is(ctx, lxb_dom_interface_element(e), name, n, &obj);
    JS_FreeCString(ctx, name);
    if (hit) return true;
    key = JS_AtomToValue(ctx, prop);
    past = fn_past_get(ctx, obj, key);
    JS_FreeValue(ctx, key);
    hit = !JS_IsUndefined(past);
    JS_FreeValue(ctx, past);
    return hit;
}

/* Web IDL §3.9.3 [[DefineOwnProperty]] step 2: a supported property name, on an object with
   [LegacyOverrideBuiltIns] and no named property setter, is refused (step 2.2.1); anything else is
   OrdinaryDefineOwnProperty (step 3). The past names map's own slot is a Symbol, so it is step 3. */
static int fn_define_own(JSContext *ctx, JSValueConst obj, JSAtom prop, JSValueConst val, JSValueConst getter,
                         JSValueConst setter, int flags)
{
    if (fn_supported(ctx, obj, prop))
        return JS_RefuseOrThrowTypeError(ctx, flags, "a form's named property has no setter and overrides its "
                                                     "built-ins, so it cannot be redefined");
    return JS_DefineProperty(ctx, obj, prop, val, getter, setter, flags | JS_PROP_NO_EXOTIC);
}

/* Web IDL §3.9.4 [[Delete]] step 2: a visible named property, with no named property deleter, is not deleted;
   quickjs reaches this only after the own properties missed, so anything else is step 4's true. */
static int fn_delete(JSContext *ctx, JSValueConst obj, JSAtom prop)
{
    return !fn_supported(ctx, obj, prop);
}

/* Web IDL §3.9.5 [[PreventExtensions]]: "Return false." */
static int fn_prevent_extensions(JSContext *ctx, JSValueConst obj)
{
    (void)ctx; (void)obj;
    return 0;
}

/* Named residuals. Not covered: (1) [[OwnPropertyKeys]]' supported property names; next diff lists HTML §4.10.3's
   sourced names in its order; absence shows as `Reflect.ownKeys(form)` naming no control. (2) HTML §4.10.3's "if an
   element listed in a form element's past names map changes form owner, then its entries must be removed"; next
   diff removes them at the form owner reset; absence shows as a moved control answering its old form's name.
   (3) A form holding a region of unknown markup (core/dom/unknown_markup.h) answers from the example's tree; next
   diff forks fn_step over the region as window.c's win_named_step does; absence shows as a `form.x` read after a
   sanitized innerHTML of attacker markup making no @S record. */
static const JSClassExoticMethods FORM_NAMED_EXOTIC = {
    .get_own_property = fn_get_own,
    .get_own_property_step = fn_get_own_step,
    /* Web IDL §3.9.1 step 2.1.2.6-8: no named property setter, so not writable; [LegacyUnenumerableNamedProperties],
       so not enumerable; configurable. */
    .get_own_property_step_flags = JS_PROP_CONFIGURABLE,
    .set_ignores_named_properties = true,
    .define_own_property = fn_define_own,
    .delete_property = fn_delete,
    .prevent_extensions = fn_prevent_extensions,
    /* A tree walk, content attribute reads, the form owner slot and the past names map: no page code. */
    .get_own_property_no_user_code = true,
};

void html_form_named_declare(JSContext *ctx)
{
    static const IdlArgType ARGS[4] = { IDL_ANY, IDL_ANY, IDL_ANY, IDL_ANY };
    JSClassDef d = { "HTMLFormElement", .exotic = &FORM_NAMED_EXOTIC };

    DCHECK(g_id_fn_step < 0, "html_form_named_declare ran twice — the class, step and slot key are the agent's");
    JS_NewClassID(JS_GetRuntime(ctx), &g_form_class);
    JS_NewClass(JS_GetRuntime(ctx), g_form_class, &d);
    /* `element`, the row whose release gives these back (core/agent_state.h): html_form_declare is reached from
       element_init. */
    agent_state_class("element", &g_form_class, "HTML §4.10.3's form wrapper class, a legacy platform object's");
    /* A form wrapper is a Node wrapper for every Node-branded argument. */
    idl_iface_implements(g_form_class, node_class_id());
    g_id_fn_step = idl_method_id_step(ctx, ARGS, 4, NULL, 0, &FN_DECL, 0);
    agent_state_id("element", &g_id_fn_step, "HTML §4.10.3 the form's named-access step declaration");
    g_past_key = JS_NewSymbol(ctx, "pastNamesMap", false);
    CHECK(!JS_IsException(g_past_key), "the past names map slot key allocation failed");
    g_atom_past = JS_ValueToAtom(ctx, g_past_key);
    CHECK(g_atom_past != JS_ATOM_NULL, "the past names map slot key could not be interned");
}

void html_form_named_free(JSRuntime *rt)
{
    JS_FreeAtomRT(rt, g_atom_past);
    g_atom_past = JS_ATOM_NULL;
    JS_FreeValueRT(rt, g_past_key);
    g_past_key = JS_UNDEFINED;
    agent_state_reached("element");
}
