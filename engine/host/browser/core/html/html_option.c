/* HTML §4.10.10 "The option element" — its SELECTEDNESS and DIRTINESS, the `selected` IDL attribute, and Web
 * IDL §3.7.2 "Legacy factory functions"'s `Option`. See core/html/html_option.h for why the two booleans are
 * not the content attribute and why §4.10.7's reset runs at the read. */
#include <stdbool.h>
#include <string.h>

#include <lexbor/dom/dom.h>

#include "check.h"
#include "quickjs.h"
#include "core/idl_args.h"
#include "core/dom/attr_list.h"
#include "core/dom/document.h"
#include "core/dom/element.h"
#include "core/dom/node.h"
#include "core/html/html_form.h"
#include "core/html/html_option.h"
#include "solver/attr_shadow.h"
#include "solver/concolic.h"
#include "solver/dom_cow.h"

/* THE TWO SLOT NAMES, spelled once. They key the (element, property) shadow, which is a map of this engine's
   own and not a property a page can see or write — the same place §4.10.5.1.15's checkedness is kept. */
#define OPT_SELECTEDNESS "selectedness"
#define OPT_DIRTINESS    "optionDirtiness"

static int  g_id_factory = -1, g_id_set_selected = -1;
static bool g_ready;

static bool opt_tag_is(const lxb_dom_node_t *n, const char *name)
{
    size_t len = 0;
    const lxb_char_t *local;

    if (!n || n->type != LXB_DOM_NODE_TYPE_ELEMENT || n->ns != LXB_NS_HTML) return false;
    local = lxb_dom_element_local_name(lxb_dom_interface_element((lxb_dom_node_t *)n), &len);
    return local && len == strlen(name) && memcmp(local, name, len) == 0;
}

/* ---- §4.10.10's two boolean states ------------------------------------------------------------------------ */

/* READ ONE OF THE TWO SLOTS. It takes no JSContext because §4.10.7's algorithm and the entry list read
   selectedness with nothing but the element in hand, and it does not COERCE: this file is the only writer of
   both slots and it writes a JS boolean, so a value of any other type is this engine's own logic having gone
   wrong rather than a page's value needing conversion — which is what the assert says instead of a ToBoolean
   that would answer `true` for it. */
static bool opt_slot(const lxb_dom_element_t *opt, const char *name, bool *out)
{
    int i = attr_shadow_find(opt, ATTR_SLOT_PROPERTY, NULL, name);
    JSValue v;

    if (i < 0) return false;
    v = attr_shadow_opaque(i);
    DCHECK(JS_IsBool(v), "§4.10.10's selectedness or dirtiness slot holds something that is not a boolean — "
                         "this component is the only writer of either, so a value of another type means a "
                         "second writer exists or the (element, property) shadow handed back another slot's");
    *out = JS_VALUE_GET_BOOL(v) != 0;
    return true;
}

bool html_option_selectedness(const lxb_dom_element_t *opt)
{
    bool slot;

    if (!opt) return false;
    /* THE ABSENT SLOT IS A POSITIVE STATEMENT and not a hole a default fills: §4.10.10 says selectedness tracks
       the `selected` attribute at every add and remove while dirtiness is false, and creation sets it from the
       attribute too — so "nothing has overridden it" and "it is whatever the attribute says" are the same fact.
       The slot exists for the states the attribute cannot express: the factory's step 6 override, and any
       selectedness a dirty option carries. */
    if (opt_slot(opt, OPT_SELECTEDNESS, &slot)) return slot;
    return dom_attr_get_ns((lxb_dom_element_t *)opt, NULL, "selected") != NULL;
}

void html_option_set_selectedness(JSContext *ctx, lxb_dom_element_t *opt, bool on)
{
    JSValue b;

    DCHECK(opt != NULL, "§4.10.10's selectedness was set on nothing — every writer reaches this with the "
                        "element in hand, and a NULL here means a walk handed back something that is not an "
                        "option element");
    /* A WRITE THAT CHANGES NOTHING RECORDS NOTHING. §4.10.7's selectedness setting algorithm runs at every read
       (see html_option.h), and each of its writes goes into the running flow's COW delta — so writing the value
       the read already answers would grow the delta by one entry per option per READ, which is a delta whose
       size is the number of times the page looked rather than the number of things it changed. */
    if (html_option_selectedness(opt) == on) return;
    b = JS_NewBool(ctx, on);
    dom_cow_set_prop_taint(ctx, opt, OPT_SELECTEDNESS, b);   /* BORROWED by the shadow, which dups it */
    JS_FreeValue(ctx, b);
}

bool html_option_dirtiness(const lxb_dom_element_t *opt)
{
    bool slot = false;

    if (!opt) return false;
    return opt_slot(opt, OPT_DIRTINESS, &slot) && slot;
}

static void option_set_dirtiness(JSContext *ctx, lxb_dom_element_t *opt)
{
    if (html_option_dirtiness(opt)) return;
    dom_cow_set_prop_taint(ctx, opt, OPT_DIRTINESS, JS_TRUE);
}

/* §4.10.10's attribute change steps — see the header for the sentence they are. */
void html_option_attr_changed(JSContext *ctx, lxb_dom_element_t *el, const char *ns, const char *local,
                              const char *old_val, const char *val)
{
    bool present;

    (void)val;
    if (ns != NULL || !local || strcmp(local, "selected") != 0) return;
    if (!opt_tag_is(lxb_dom_interface_node(el), "option")) return;
    if (html_option_dirtiness(el)) return;   /* "if its dirtiness is false" — the whole of what dirtiness does */
    /* ADDED and REMOVED ARE READ OFF THE ELEMENT AND NOT OFF THE VALUE POINTERS, because a NULL value pointer
       is AMBIGUOUS here and presence is not. The chokepoint's old value is `lxb_dom_attr_value`'s, which
       answers NULL both for an attribute that was ABSENT and for one that was present carrying no value string
       at all — which is exactly what a parser-created boolean `<option selected>` is. This hook runs AFTER the
       write, so the element itself answers the new state exactly.
       WHICH LEAVES ONE QUESTION THE POINTERS DO DECIDE: with the attribute present afterwards, was this an ADD
       or a value overwrite? A non-NULL old value settles it — the attribute was there, so nothing was added
       and the steps name nothing to do. A NULL old value is the ambiguous pair above, and both members of it
       reach the same answer: a genuine add sets selectedness true, and an overwrite of a value-less
       `<option selected>` on an option whose dirtiness is false has selectedness true ALREADY (the slot is
       absent and the read falls through to the attribute), so the write below records nothing. The one state
       that could disagree — selectedness false under a present attribute — is reachable only through this
       file's own factory, whose step 5 writes a real empty string, and through the `selected` setter, which
       sets dirtiness and returned above. */
    present = dom_attr_get_ns(el, NULL, "selected") != NULL;
    if (present && old_val != NULL) return;   /* a value overwrite: neither added nor removed */
    html_option_set_selectedness(ctx, el, present);
}

void html_option_ask_for_a_reset(JSContext *ctx, lxb_dom_element_t *opt)
{
    lxb_dom_node_t *select;

    if (!opt) return;
    select = html_form_select_of_option(ctx, lxb_dom_interface_node(opt));
    if (select) html_form_selectedness_setting_algorithm(ctx, select);
}

/* ---- §4.10.10's `selected` IDL attribute ------------------------------------------------------------------ */

static lxb_dom_element_t *option_receiver(JSContext *ctx, JSValueConst this_val)
{
    lxb_dom_node_t *n = node_of(this_val);

    if (opt_tag_is(n, "option")) return lxb_dom_interface_element(n);
    JS_ThrowTypeError(ctx, "HTMLOptionElement.selected was reached on something that is not an option");
    return NULL;
}

/* "The selected IDL attribute, on getting, must return true if the element's selectedness is true, and false
   otherwise." The reset runs first — see html_option.h for why that is this engine's spelling of §4.10.7's own
   invocation points rather than a narrowing of them. */
static JSValue js_option_get_selected(JSContext *ctx, JSValueConst this_val, int magic)
{
    lxb_dom_element_t *el = option_receiver(ctx, this_val);

    (void)magic;
    if (!el) return JS_EXCEPTION;
    html_option_ask_for_a_reset(ctx, el);
    return JS_NewBool(ctx, html_option_selectedness(el));
}

/* "On setting, it must set the element's selectedness to the new value, set its dirtiness to true, and then
   cause the element to ask for a reset." Three steps, in that order: the reset reads the selectedness this
   setter just wrote, and it reads the DIRTINESS of every option it visits, so a reset run before either write
   would be run against the state the assignment replaced. */
static JSValue js_option_set_selected(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic)
{
    lxb_dom_element_t *el = option_receiver(ctx, this_val);

    (void)magic;
    if (!el) return JS_EXCEPTION;
    DCHECK(JS_IsBool(val), "§4.10.10's `selected` setter reached its body unconverted — Web IDL §3.2.3 boolean "
                           "is the declaration's conversion and it FORKS an unknown value there, so a body that "
                           "coerced it here would answer `true` for every concolic and collapse the world the "
                           "fork exists to keep");
    html_option_set_selectedness(ctx, el, JS_ToBool(ctx, val) != 0);
    option_set_dirtiness(ctx, el);
    html_option_ask_for_a_reset(ctx, el);
    return JS_UNDEFINED;
}

/* ---- Web IDL §3.7.2's `Option` ---------------------------------------------------------------------------- */

/* HTML §4.10.10's own seven steps, verbatim:
 *   1. "Let document be the current global object's associated Document."
 *   2. "Let option be the result of creating an element given document, "option", and the HTML namespace."
 *   3. "If text is not the empty string, then append to option a new Text node whose data is text."
 *   4. "If value is given, then set an attribute value for option using "value" and value."
 *   5. "If defaultSelected is true, then set an attribute value for option using "selected" and the empty
 *      string."
 *   6. "If selected is true, then set option's selectedness to true; otherwise set its selectedness to false
 *      (even if defaultSelected is true)."
 *   7. "Return option."
 *
 * STEP 6 IS WHY THIS FILE EXISTS. It sets selectedness EXPLICITLY, after step 5 has added the very attribute
 * the attribute change steps read as "set selectedness to true" — and the standard says so in its own note:
 * "The Option() constructor, when called with three or fewer arguments, overrides the initial state of the
 * selectedness state to always be false even if the third argument is true (implying that a selected attribute
 * is to be set). The fourth argument can be used to explicitly set the initial selectedness state when using
 * the constructor." So `new Option("a", "b", true)` serializes as `<option value="b" selected>a</option>` and
 * answers `selected === false`, which the content attribute alone cannot say.
 *
 * STEP 4 IS "IF GIVEN", AND THAT IS ARGV AND NOT ARGC HERE. `optional DOMString value` carries NO default,
 * while positions 2 and 3 do — so Web IDL §3.6 "Overload resolution algorithm" step 16.1 places those defaults
 * and the argument count reaches this body at 4 for every call. What tells "given" from "not given" is the
 * position itself: §3.6 step 15.4.2 and step 16.2 both append "the special value 'missing'" for an optional
 * argument with no declared default, which the declaration delivers as `undefined`. `new Option("a")` and
 * `new Option("a", undefined)` are therefore the SAME call, which is what §3.6 says of every such position —
 * and an argc test would have set `value="undefined"` on the second of them.
 *
 * STEP 3 IS AN APPEND AND NOT `textContent =`. On a freshly created element the two have the same effect, and
 * they are still not the same algorithm — DOM §4.4 "Interface Node"'s string replace all removes the children
 * first — so this is the append the standard names, through the mutation chokepoint that owns the per-flow
 * delta. The empty-string test is the spec's own: `new Option()` has NO child node at all, which a page reading
 * `firstChild` or `childNodes.length` can see. */
static JSValue js_option_factory(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    JSValue option, empty;
    lxb_dom_element_t *el;
    lxb_dom_text_t *text;
    const char *str = NULL;
    size_t len = 0;
    bool owned_cstr = false;

    (void)magic;
    /* Web IDL §3.7.2 step 1 of the function's steps: "If NewTarget is undefined, then throw a TypeError."
       JS_CFUNC_step_ctor delivers NEW_TARGET in the receiver slot and undefined for a plain call, which is how
       `Option()` is told apart from `new Option()`. */
    if (JS_IsUndefined(this_val))
        return JS_ThrowTypeError(ctx, "Failed to construct 'Option': please use the 'new' operator");
    DCHECK(argc == 4, "§4.10.10's legacy factory function reached its body at an arity its declaration does "
                      "not produce — three of its four positions carry a declared default, so Web IDL §3.6 "
                      "step 16.1 places one at every position the page stopped short of and the count is 4 for "
                      "every call");
    option = document_create_element_internal(ctx, "option", 6);        /* steps 1-2 */
    if (JS_IsException(option)) return option;
    el = element_of_value(option);
    DCHECK(opt_tag_is(lxb_dom_interface_node(el), "option"),
           "HTML §4.10.10's legacy factory function created something that is not an option element — DOM §4.9 "
           "Interface Element's create an element was given the local name option in the HTML namespace, so a "
           "node that is not one means the element-interface resolution and this file disagree about what an "
           "option element is");

    /* Step 3. THE TEXT CROSSES AS THE VALUE: `new Option(row.label, row.id)` over an API reply carries what the
       run learned, so an unknown external input keeps its provenance in the (element, "textContent") shadow
       while the Text node holds the shape — the same pair DOM §4.4's `textContent` setter writes, and the same
       one core/dom/node.c's getter reads back. */
    if (concolic_is(argv[0])) {
        str = concolic_shape_c(argv[0]);
        if (!str) str = "";
        len = strlen(str);
        if (len) dom_cow_set_prop_taint(ctx, el, "textContent", argv[0]);
    } else {
        DCHECK(JS_IsString(argv[0]),
               "§4.10.10's `text` argument reached the body unconverted — the IDL declaration is what converts "
               "it, and running the page's own toString from here is the drive-to-completion the flow "
               "machinery exists to avoid");
        str = JS_ToCStringLen(ctx, &len, argv[0]);
        if (!str) { JS_FreeValue(ctx, option); return JS_EXCEPTION; }
        owned_cstr = true;
    }
    if (len) {   /* "If text is not the empty string" */
        text = lxb_dom_document_create_text_node(lxb_dom_interface_node(el)->owner_document,
                                                 (const lxb_char_t *)str, len);
        DCHECK(text != NULL, "§4.10.10 step 3's Text node was not created — the option's text would silently "
                             "not be there, and `option.text` would answer the empty string for a page that "
                             "asked for one with content");
        if (text) {
            dom_cow_note_created(lxb_dom_interface_node(text));   /* this flow made it: the delta owns it */
            dom_cow_append_child(lxb_dom_interface_node(el), lxb_dom_interface_node(text));
        }
    }
    if (owned_cstr) JS_FreeCString(ctx, str);

    /* Step 4. THE VALUE CROSSES AS THE VALUE — element_attr_set_value carries the whole triple into the
       (element, name) shadow, so an id a page took out of an API reply reaches §4.10.22.4's entry list as the
       source it came from rather than as bytes this file formatted. */
    if (!JS_IsUndefined(argv[1]))
        element_attr_set_value(ctx, option, "value", argv[1]);

    DCHECK(JS_IsBool(argv[2]) && JS_IsBool(argv[3]),
           "§4.10.10's `defaultSelected`/`selected` reached the body unconverted — Web IDL §3.2.3 boolean is "
           "the declaration's conversion and it FORKS an unknown value there, so a body coercing one here "
           "would answer `true` for every concolic and collapse the world the fork exists to keep");
    if (JS_ToBool(ctx, argv[2])) {   /* step 5 */
        empty = JS_NewString(ctx, "");
        if (JS_IsException(empty)) { JS_FreeValue(ctx, option); return empty; }
        element_attr_set_value(ctx, option, "selected", empty);
        JS_FreeValue(ctx, empty);
    }
    /* Step 6, and it runs AFTER step 5 because that is the order the steps are numbered in: step 5's write
       fires §4.10.10's attribute change steps, which set selectedness to true, and this is the "(even if
       defaultSelected is true)" that overrides them. Dirtiness is deliberately NOT set — the factory's steps
       never name it, so a later add or remove of the attribute still moves selectedness. */
    html_option_set_selectedness(ctx, el, JS_ToBool(ctx, argv[3]) != 0);
    return option;                                                      /* step 7 */
}

/* ---- declare and install ---------------------------------------------------------------------------------- */

void html_option_declare(JSContext *ctx)
{
    static const IdlArgType FACTORY_ARGS[4] = { IDL_DOMSTRING, IDL_DOMSTRING, IDL_BOOLEAN, IDL_BOOLEAN };

    DCHECK(!g_ready, "html_option_declare ran twice — §4.10.10's declarations are made once per AGENT, and a "
                     "second one would leave the member ids every realm's `Option` and `selected` were minted "
                     "with naming declarations nothing else refers to");
    g_id_set_selected = idl_setter_id(ctx, IDL_BOOLEAN, false, js_option_set_selected, 0);
    /* `Option(optional DOMString text = "", optional DOMString value, optional boolean defaultSelected = false,
       optional boolean selected = false)` — every position optional, which is what makes the function object's
       `length` 0 (Web IDL §3.7.2: "Let length be the length of the shortest argument list of the entries in
       S"). THE DEFAULTS ARE DECLARED AND NOT RE-DERIVED IN THE BODY: `text`'s `= ""` is why step 3's
       empty-string test is the whole of what `new Option()` needs, and the two booleans' `= false` is why
       steps 5 and 6 read a real `false` rather than inventing one from an absence. `value` has NO default, and
       that omission is the declaration — it is what makes step 4's "if value is given" answerable. */
    g_id_factory = idl_method_id(ctx, FACTORY_ARGS, 4, js_option_factory, 0);
    idl_optional_from(0);
    idl_arg_default(0, IDL_DEFAULT_STRING, "");     /* §3.6 steps 15.4.1 and 16.1's `= ""` */
    idl_arg_default(2, IDL_DEFAULT_FALSE, NULL);    /* §3.6 steps 15.4.1 and 16.1's `= false` */
    idl_arg_default(3, IDL_DEFAULT_FALSE, NULL);
    g_ready = true;
}

void html_option_install_members(JSContext *ctx, JSValueConst option_proto)
{
    DCHECK(g_ready, "§4.10.10's `selected` was installed before its setter was declared");
    DCHECK(JS_IsObject(option_proto),
           "§4.10.10's members were installed with no HTMLOptionElement.prototype");
    idl_install_accessor(ctx, option_proto, "selected", js_option_get_selected, 0, g_id_set_selected);
}

void html_option_install_global(JSContext *ctx, JSValueConst global, JSValueConst proto)
{
    JSValue factory;

    DCHECK(g_ready, "§4.10.10's legacy factory function was installed before its argument list was declared");
    DCHECK(JS_IsObject(proto),
           "§4.10.10's legacy factory function was installed with no HTMLOptionElement.prototype — Web IDL "
           "§3.7.2 gives F a non-configurable `prototype` naming the interface prototype object of THIS "
           "realm, and a factory whose `prototype` is not that object makes `new Option() instanceof "
           "HTMLOptionElement` and `Option.prototype === HTMLOptionElement.prototype` disagree");
    /* Web IDL §3.7.2: "Let F be CreateBuiltinFunction(steps, length, id, « », realm)", with length 0 and id
       `Option`. It is a step-declared constructor so each `DOMString` is converted by the declaration —
       running the page's own `toString` — before the body is entered, and so a call without `new` arrives with
       an undefined receiver. */
    factory = idl_step_constructor(ctx, "Option", g_id_factory);
    CHECK(!JS_IsException(factory), "§4.10.10: the `Option` legacy factory function could not be allocated");
    /* "Perform ! DefinePropertyOrThrow(F, "prototype", PropertyDescriptor{[[Value]]: proto, [[Writable]]:
       false, [[Enumerable]]: false, [[Configurable]]: false})." NOT JS_SetConstructor, which would also write
       `HTMLOptionElement.prototype.constructor = Option` — a legacy factory function is not the interface's
       constructor, and `Option.prototype.constructor` must stay `HTMLOptionElement`. Flags 0 is exactly
       {[[Writable]]: false, [[Enumerable]]: false, [[Configurable]]: false}. */
    JS_DefinePropertyValueStr(ctx, factory, "prototype", JS_DupValue(ctx, proto), 0);
    JS_SetPropertyStr(ctx, (JSValue)global, "Option", factory);
}

void html_option_free(JSRuntime *rt)
{
    (void)rt;
    if (!g_ready) return;
    /* The member ids belong to the argument pool core/idl_args.c releases with the agent; what this component
       owns is the statement that the declarations HAPPENED, so that is what is given back. */
    g_ready = false;
    g_id_factory = -1;
    g_id_set_selected = -1;
}
