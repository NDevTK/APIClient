/* HTML's ENUMERATED GLOBAL ATTRIBUTES — the HTMLElement members that COMPUTE a value from the tree.
 *
 * WHY THEY ARE NOT REFLECTIONS AND CANNOT BE. core/dom/element.c's ElReflect registry mirrors ONE attribute in
 * both directions: the IDL property IS the attribute. Every member here answers a question the attribute alone
 * does not: `translate` is true unless the nearest ancestor that states a translation mode states `no`,
 * `spellcheck` and `writingSuggestions` inherit the same way, `autocorrect` inherits from a form OWNER rather
 * than from a parent, `autocapitalize` inherits from a form owner AND cannot name its own answer (§6.8.7 maps
 * two keywords to None and two to Sentences, so §2.3.3's canonical keyword is undefined for both states and the
 * section states the getter's answer for each), and `isContentEditable` is a property of the element's position
 * in an editing host. A
 * REFLECT_BOOL for any of them would answer the attribute's PRESENCE — `<div translate=no>` would read `true`
 * because the attribute is there — which is the failure html_style_element.c records for `<style disabled>`:
 * wrong in both directions, silently. §6.11.7's `draggable` is the same trap in its sharpest form: the state
 * an author writes is `true`/`false` TEXT, so a boolean reflection would answer `true` for `<img draggable=false>`
 * — the exact opposite of what the page said — and would answer false for the `<img>` and the `<a href>` whose
 * Auto state the section says are draggable.
 *
 * EACH IS AN ENUMERATED ATTRIBUTE — HTML §2.3.3 Keywords and enumerated attributes — whose determine-the-state
 * algorithm is core/html/enumerated_attribute.c, asked here with each attribute's own keyword table and its own
 * three special states. What every table below has to get right is which state goes in which position: the
 * keyword match comes BEFORE the empty check because §2.3.3 puts it there, and `translate=""` (Yes) versus
 * `translate="maybe"` (Inherit) is the pair that tells the empty and invalid value defaults apart.
 *
 * THE WALKS ARE ANCESTOR WALKS AND THEY ARE NOT BOUNDED. A tree is finite and the walk is one step per level;
 * §NO BOUNDS is about work items, not about the height of a document. */
#include <stdbool.h>
#include <string.h>

#include <lexbor/dom/dom.h>

#include "check.h"
#include "quickjs.h"
#include "solver/dom_cow.h"
#include "core/dom/element.h"
#include "core/html/enumerated_attribute.h"
#include "core/html/global_attributes.h"
#include "core/html/html_element.h"
#include "core/html/html_form.h"
#include "core/idl_args.h"

static int g_id_set_translate = -1;
static int g_id_set_spellcheck = -1;
static int g_id_set_writing_suggestions = -1;
static int g_id_set_autocapitalize = -1;
static int g_id_set_autocorrect = -1;
static int g_id_set_content_editable = -1;
static int g_id_set_draggable = -1;
static bool g_ready;

/* §2.3.3's DETERMINE THE STATE was written here, and it is now core/html/enumerated_attribute.c — this file's
   header still says the algorithm is written once, and that is only true of a component every asker can reach.
   The copy that stood here was complete (all four steps, all three special states) and was still the wrong
   shape, because a `static` cannot be a second caller's answer: §2.5.5's referrer policy attribute would have
   been the fifth hand-rolled copy in this tree rather than the first reuse of this one. */

/* The element's PARENT ELEMENT — DOM §4.4's "parent element", which is null when the parent is not an element
   (a Document or a DocumentFragment), and which is what every inheritance below walks. */
static lxb_dom_element_t *parent_element(lxb_dom_element_t *el)
{
    lxb_dom_node_t *p = lxb_dom_interface_node(el)->parent;

    return (p && p->type == LXB_DOM_NODE_TYPE_ELEMENT) ? lxb_dom_interface_element(p) : NULL;
}

static bool element_is_html(lxb_dom_element_t *el)
{
    return lxb_dom_interface_node(el)->ns == LXB_NS_HTML;
}

/* An HTML element's local name, ASCII-lowercase already for an HTML-namespace element. */
static bool local_name_is(lxb_dom_element_t *el, const char *name)
{
    size_t len = 0;
    const lxb_char_t *n = lxb_dom_element_local_name(el, &len);

    return n && strlen(name) == len && memcmp(n, name, len) == 0;
}

/* WEB IDL §3.7.6 Attributes' brand check, a THROW and not an assert: every member below sits on
   HTMLElement.prototype and
   a page reaches an accessor off a prototype with `.call` on anything at all. */
static lxb_dom_element_t *receiver(JSContext *ctx, JSValueConst this_val, const char *member)
{
    if (html_element_is(this_val)) {
        lxb_dom_element_t *el = element_of_value(this_val);

        /* html_element_is already asked node_of for an ELEMENT node in the HTML namespace, so the two answers
           cannot disagree — and a NULL here would return JS_EXCEPTION with no exception pending. */
        DCHECK(el != NULL, "an HTML element wrapper had no element behind it");
        return el;
    }
    JS_ThrowTypeError(ctx, "HTMLElement.%s was reached on something that is not an HTML element", member);
    return NULL;
}

/* ---- HTML §3.2.6.3 The translate attribute ---------------------------------------------------------------- */

enum { TRANSLATE_YES, TRANSLATE_NO, TRANSLATE_INHERIT };
static const EnumeratedKeyword TRANSLATE_KW[] = {
    { "yes", TRANSLATE_YES }, { "no", TRANSLATE_NO }, { NULL, 0 }
};

/* §3.2.6.3's TRANSLATION MODE: Yes is translate-enabled, No is no-translate, and otherwise — the Inherit state,
   OR an element that is not an HTML element and therefore has no translate attribute — it is the parent
   element's, or translate-enabled when the parent element is null. Every element has one, non-HTML included,
   which is why the namespace test is inside the loop rather than a precondition. */
static bool translation_enabled(lxb_dom_element_t *el)
{
    for (; el; el = parent_element(el)) {
        if (!element_is_html(el)) continue;
        switch (enumerated_attribute_state(el, "translate", TRANSLATE_KW,
                                           TRANSLATE_INHERIT, TRANSLATE_YES, TRANSLATE_INHERIT)) {
        case TRANSLATE_YES: return true;
        case TRANSLATE_NO:  return false;
        default:            break;
        }
    }
    return true;
}

static JSValue js_translate_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    lxb_dom_element_t *el = receiver(ctx, this_val, "translate");

    (void)magic;
    if (!el) return JS_EXCEPTION;
    return JS_NewBool(ctx, translation_enabled(el));
}

/* §3.2.6.3: "On setting, it must set the content attribute's value to `yes` if the new value is true, and set
   the content attribute's value to `no` otherwise." */
static JSValue js_translate_set(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic)
{
    (void)magic;
    if (!receiver(ctx, this_val, "translate")) return JS_EXCEPTION;
    element_attr_set(ctx, this_val, "translate", JS_ToBool(ctx, val) ? "yes" : "no");
    return JS_UNDEFINED;
}

/* ---- HTML §6.8.5 Spelling and grammar checking ------------------------------------------------------------ */

enum { SPELLCHECK_TRUE, SPELLCHECK_FALSE, SPELLCHECK_DEFAULT };
static const EnumeratedKeyword SPELLCHECK_KW[] = {
    { "true", SPELLCHECK_TRUE }, { "false", SPELLCHECK_FALSE }, { NULL, 0 }
};

/* §6.8.5 requires the USER AGENT to establish a default behavior per element, out of true-by-default,
   false-by-default and inherit-by-default, and says only that an element with no parent element cannot be
   inherit-by-default. THIS AGENT'S CHOICE, stated here because the section makes it ours: inherit-by-default
   for an element that has a parent element, true-by-default for one that does not. That is the choice the
   getter's recursion collapses to the loop below — walk until an element STATES true or false, and answer true
   at the root — and it is the behaviour a page reads back from a detached `document.createElement("div")`. */
static bool spellcheck_enabled(lxb_dom_element_t *el)
{
    for (; el; el = parent_element(el)) {
        switch (enumerated_attribute_state(el, "spellcheck", SPELLCHECK_KW,
                                           SPELLCHECK_DEFAULT, SPELLCHECK_TRUE, SPELLCHECK_DEFAULT)) {
        case SPELLCHECK_TRUE:  return true;
        case SPELLCHECK_FALSE: return false;
        default:               break;   /* Default: inherit-by-default while there is a parent element */
        }
    }
    return true;                        /* no parent element: true-by-default */
}

static JSValue js_spellcheck_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    lxb_dom_element_t *el = receiver(ctx, this_val, "spellcheck");

    (void)magic;
    if (!el) return JS_EXCEPTION;
    return JS_NewBool(ctx, spellcheck_enabled(el));
}

/* §6.8.5: "On setting, if the new value is true, then the element's spellcheck content attribute must be set to
   `true`, otherwise it must be set to `false`." */
static JSValue js_spellcheck_set(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic)
{
    (void)magic;
    if (!receiver(ctx, this_val, "spellcheck")) return JS_EXCEPTION;
    element_attr_set(ctx, this_val, "spellcheck", JS_ToBool(ctx, val) ? "true" : "false");
    return JS_UNDEFINED;
}

/* ---- HTML §6.8.6 Writing suggestions ---------------------------------------------------------------------- */

enum { WS_TRUE, WS_FALSE, WS_DEFAULT };
static const EnumeratedKeyword WS_KW[] = { { "true", WS_TRUE }, { "false", WS_FALSE }, { NULL, 0 } };

/* §6.8.6's COMPUTED WRITING SUGGESTIONS VALUE, whose recursion is over the parent element and stops the moment
   an element states a value: False is "false"; Default recurses and is "false" only if the parent's computed
   value is; anything else is "true". Note the DEFAULTS are not spellcheck's — the missing value default is
   Default while the INVALID and EMPTY value defaults are both True. */
static bool writing_suggestions_offered(lxb_dom_element_t *el)
{
    for (; el; el = parent_element(el)) {
        int st = enumerated_attribute_state(el, "writingsuggestions", WS_KW, WS_DEFAULT, WS_TRUE, WS_TRUE);

        if (st == WS_FALSE) return false;
        if (st != WS_DEFAULT) return true;
    }
    return true;
}

/* §6.8.6: "The writingSuggestions getter steps are: Return this's computed writing suggestions value" — a
   DOMString "true" or "false", not a boolean, which is what the IDL says and what a page compares against. */
static JSValue js_writing_suggestions_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    lxb_dom_element_t *el = receiver(ctx, this_val, "writingSuggestions");

    (void)magic;
    if (!el) return JS_EXCEPTION;
    return JS_NewString(ctx, writing_suggestions_offered(el) ? "true" : "false");
}

/* The IDL is `[CEReactions, ReflectSetter] attribute DOMString writingSuggestions`: the SETTER is the plain
   reflection setter — it writes the string it is given into the content attribute — while the getter is the
   computed value above. That asymmetry is why this member cannot be an ElReflect row. */
static JSValue js_writing_suggestions_set(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic)
{
    const char *s;

    (void)magic;
    if (!receiver(ctx, this_val, "writingSuggestions")) return JS_EXCEPTION;
    s = JS_ToCString(ctx, val);
    if (!s) return JS_EXCEPTION;
    element_attr_set(ctx, this_val, "writingsuggestions", s);
    JS_FreeCString(ctx, s);
    return JS_UNDEFINED;
}

/* §4.10.2 Categories — the AUTOCAPITALIZE-AND-AUTOCORRECT INHERITING ELEMENTS, "elements that inherit the
   autocapitalize and autocorrect attributes from their form owner". BOTH sections below ask this one question
   and the standard names the category after both attributes, which is why it is not spelled `autocorrect_`
   anything: §6.8.7 step 2 and §6.8.8 step 3 are the same list, and a second copy under the other name is how
   the two would come to differ. */
static bool autocapitalize_and_autocorrect_inheriting(lxb_dom_element_t *el)
{
    return element_is_html(el) &&
           (local_name_is(el, "button") || local_name_is(el, "fieldset") || local_name_is(el, "input") ||
            local_name_is(el, "output") || local_name_is(el, "select") || local_name_is(el, "textarea"));
}

/* ---- HTML §6.8.7 Autocapitalization ------------------------------------------------------------------------ */

/* §6.8.7's FIVE AUTOCAPITALIZATION HINTS, and the mapping that is the whole reason this member is here rather
   than a REFLECT_ENUM row in core/dom/element.c: TWO keywords map to None and TWO to Sentences. §2.3.3's
   canonical keyword is defined only where "there is only one keyword mapping to the given state", so those two
   states have no canonical keyword this engine may pick — and §6.8.7 does not leave that to be guessed, it
   names the answer for each of them in the getter steps below. */
enum { AC_DEFAULT = 0, AC_NONE, AC_SENTENCES, AC_WORDS, AC_CHARACTERS };
static const EnumeratedKeyword AUTOCAPITALIZE_KW[] = {
    { "off", AC_NONE },       { "none", AC_NONE },
    { "on", AC_SENTENCES },   { "sentences", AC_SENTENCES },
    { "words", AC_WORDS },    { "characters", AC_CHARACTERS },
    { NULL, 0 }
};

/* §6.8.7's OWN AUTOCAPITALIZATION HINT, in the section's three steps.
   STEP 1 IS NOT §2.3.3'S STEP 1. It reads "if the autocapitalize content attribute is present on element, AND
   ITS VALUE IS NOT THE EMPTY STRING, return the state of the attribute" — so `<input autocapitalize="">` does
   NOT take the attribute's empty value default, it falls through to the form owner, which is a condition no
   keyword table can express and the reason presence and emptiness are tested here rather than passed as
   defaults. Once step 1 has decided the attribute is there and non-empty, §2.3.3 decides which state, and only
   its INVALID value default (Sentences) can still be reached.
   THE RECURSION IS ONE HOP AND THAT IS PROVABLE, not a bound: step 2 recurses into the FORM OWNER, a form owner
   is a `form` element, and `form` is not one of §4.10.2's autocapitalize-and-autocorrect inheriting elements —
   so the owner's own step 2 cannot fire. Asserted, because the day that list gains `form` this reads wrong. */
static int own_autocapitalization_hint(JSContext *ctx, JSValueConst wrap, lxb_dom_element_t *el)
{
    JSValue owner;
    lxb_dom_element_t *form;
    int hint = AC_DEFAULT;
    size_t len = 0;

    if (lxb_dom_element_has_attribute(el, (const lxb_char_t *)"autocapitalize", 14) &&        /* step 1 */
        lxb_dom_element_get_attribute(el, (const lxb_char_t *)"autocapitalize", 14, &len) && len != 0)
        return enumerated_attribute_state(el, "autocapitalize", AUTOCAPITALIZE_KW,
                                          AC_DEFAULT, AC_SENTENCES, AC_SENTENCES);
    if (!autocapitalize_and_autocorrect_inheriting(el)) return AC_DEFAULT;              /* step 3 */
    owner = html_form_owner_of(ctx, wrap);                                                    /* step 2 */
    form = element_of_value(owner);
    if (form) {
        DCHECK(!autocapitalize_and_autocorrect_inheriting(form),
               "§6.8.7 step 2 recursed into a form owner that is itself an autocapitalize-and-autocorrect "
               "inheriting element — §4.10.2's list is button/fieldset/input/output/select/textarea and a form "
               "owner is a `form`, so this walk is one hop by construction and would need a loop if it were not");
        len = 0;
        if (lxb_dom_element_has_attribute(form, (const lxb_char_t *)"autocapitalize", 14) &&
            lxb_dom_element_get_attribute(form, (const lxb_char_t *)"autocapitalize", 14, &len) && len != 0)
            hint = enumerated_attribute_state(form, "autocapitalize", AUTOCAPITALIZE_KW,
                                              AC_DEFAULT, AC_SENTENCES, AC_SENTENCES);
    }
    JS_FreeValue(ctx, owner);
    return hint;
}

/* §6.8.7: "The autocapitalize getter steps are to: Let state be the own autocapitalization hint of this. If
   state is Default, then return the empty string. If state is None, then return "none". If state is Sentences,
   then return "sentences". Return the keyword value corresponding to state."
   THIS MEMBER WAS A `REFLECT_STRING` ROW ON HTMLElement, which mirrored the attribute's raw bytes — so
   `<div autocapitalize="OFF">.autocapitalize` answered "OFF" where these steps answer "none", an element
   inheriting from its form owner answered "" where they answer the owner's keyword, and every unrecognised
   value answered itself where they answer "sentences". */
static JSValue js_autocapitalize_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    lxb_dom_element_t *el = receiver(ctx, this_val, "autocapitalize");
    int state;

    (void)magic;
    if (!el) return JS_EXCEPTION;
    state = own_autocapitalization_hint(ctx, this_val, el);
    if (state == AC_DEFAULT) return JS_NewStringLen(ctx, "", 0);
    if (state == AC_NONE) return JS_NewString(ctx, "none");
    if (state == AC_SENTENCES) return JS_NewString(ctx, "sentences");
    return JS_NewString(ctx, enumerated_attribute_canonical_keyword(AUTOCAPITALIZE_KW, state));
}

/* The IDL is `[CEReactions, ReflectSetter] attribute DOMString autocapitalize`, so the SETTER is §2.6.1's plain
   "set the content attribute with the given value" while the getter above is the section's own algorithm — the
   same asymmetry `writingSuggestions` has, and the same reason neither can be a registry row. */
static JSValue js_autocapitalize_set(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic)
{
    const char *s;

    (void)magic;
    if (!receiver(ctx, this_val, "autocapitalize")) return JS_EXCEPTION;
    s = JS_ToCString(ctx, val);
    if (!s) return JS_EXCEPTION;
    element_attr_set(ctx, this_val, "autocapitalize", s);
    JS_FreeCString(ctx, s);
    return JS_UNDEFINED;
}

/* ---- HTML §6.8.8 Autocorrection --------------------------------------------------------------------------- */

enum { AUTOCORRECT_ON, AUTOCORRECT_OFF };
static const EnumeratedKeyword AUTOCORRECT_KW[] = {
    { "on", AUTOCORRECT_ON }, { "off", AUTOCORRECT_OFF }, { NULL, 0 }
};

/* §4.10.5.1's URL, Email and Password states of an `input` element's `type` attribute — the three §6.8.8 step 1
   names, and the only three of that enumeration this file has any question about. */
static bool input_type_bars_autocorrect(lxb_dom_element_t *el)
{
    static const EnumeratedKeyword TYPE_KW[] = { { "url", 1 }, { "email", 1 }, { "password", 1 }, { NULL, 0 } };

    if (!element_is_html(el) || !local_name_is(el, "input")) return false;
    /* The missing, empty and invalid value defaults are all the Text state, which is not one of the three. */
    return enumerated_attribute_state(el, "type", TYPE_KW, 0, 0, 0) == 1;
}

/* §6.8.8's USED AUTOCORRECTION STATE. Step 3 is the form-owner arm, and the owner is asked for by RUNNING
   §4.10.18.3's association rather than by reading a stored field — core/html/html_form.c owns that and says
   why. Its answer is a WRAPPER, so it is the one place here that needs one. */
static bool autocorrect_on(JSContext *ctx, JSValueConst wrap, lxb_dom_element_t *el)
{
    JSValue owner;
    lxb_dom_element_t *form;
    bool on;

    if (input_type_bars_autocorrect(el)) return false;                              /* step 1 */
    if (lxb_dom_element_has_attribute(el, (const lxb_char_t *)"autocorrect", 11))   /* step 2 */
        return enumerated_attribute_state(el, "autocorrect", AUTOCORRECT_KW,
                                          AUTOCORRECT_ON, AUTOCORRECT_ON, AUTOCORRECT_ON) == AUTOCORRECT_ON;
    if (!autocapitalize_and_autocorrect_inheriting(el)) return true;                                   /* step 4 */
    owner = html_form_owner_of(ctx, wrap);                                          /* step 3 */
    form = element_of_value(owner);
    on = form ? enumerated_attribute_state(form, "autocorrect", AUTOCORRECT_KW,
                                           AUTOCORRECT_ON, AUTOCORRECT_ON, AUTOCORRECT_ON) == AUTOCORRECT_ON
              : true;
    JS_FreeValue(ctx, owner);
    return on;
}

static JSValue js_autocorrect_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    lxb_dom_element_t *el = receiver(ctx, this_val, "autocorrect");

    (void)magic;
    if (!el) return JS_EXCEPTION;
    return JS_NewBool(ctx, autocorrect_on(ctx, this_val, el));
}

/* §6.8.8: "The setter steps are: if the given value is true, then the element's autocorrect attribute must be
   set to `on`; otherwise it must be set to `off`." */
static JSValue js_autocorrect_set(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic)
{
    (void)magic;
    if (!receiver(ctx, this_val, "autocorrect")) return JS_EXCEPTION;
    element_attr_set(ctx, this_val, "autocorrect", JS_ToBool(ctx, val) ? "on" : "off");
    return JS_UNDEFINED;
}

/* ---- HTML §6.8.1 Making document regions editable: The contenteditable content attribute ------------------- */

enum { CE_TRUE, CE_FALSE, CE_PLAINTEXT_ONLY, CE_INHERIT };
static const EnumeratedKeyword CE_KW[] = {
    { "true", CE_TRUE }, { "false", CE_FALSE }, { "plaintext-only", CE_PLAINTEXT_ONLY }, { NULL, 0 }
};

static int content_editable_state(lxb_dom_element_t *el)
{
    return enumerated_attribute_state(el, "contenteditable", CE_KW, CE_INHERIT, CE_TRUE, CE_INHERIT);
}

/* §6.8.1: "an editing host is either an HTML element with its contenteditable attribute in the true state or
   plaintext-only state, or a child HTML element of a Document whose design mode enabled is true."
   THE SECOND ARM IS UNREACHABLE IN THIS ENGINE AND THAT IS A FACT ABOUT Document, NOT A SHORTCUT HERE: §6.8.2
   makes `design mode enabled` a Document boolean "initially false" that only the `designMode` setter changes,
   and this engine's Document does not install `designMode` — the IDL audit names it among Document's absent
   members. Building §6.8.2 is what makes the arm reachable, and this is one of the readers it has to reach. */
static bool is_editing_host(lxb_dom_element_t *el)
{
    int st;

    if (!element_is_html(el)) return false;
    st = content_editable_state(el);
    return st == CE_TRUE || st == CE_PLAINTEXT_ONLY;
}

/* execCommand's "Something is editable if it is a node; it is not an editing host; it does not have a
   contenteditable attribute set to the false state; its parent is an editing host or editable; and either it is
   an HTML element, or it is an svg or math element, or it is not an Element and its parent is an HTML element."
   Written as a walk UP rather than a recursion down, which is the same predicate read backwards: an element is
   editable when the nearest ancestor that states anything states true or plaintext-only, no element between
   states false, and every step of the chain is an element the definition admits. */
static bool is_editable(lxb_dom_element_t *el)
{
    lxb_dom_element_t *e;

    if (is_editing_host(el)) return false;   /* "it is not an editing host" */
    for (e = el; e; e = parent_element(e)) {
        lxb_dom_element_t *p;

        if (e != el && is_editing_host(e)) return true;   /* the parent chain reached an editing host */
        if (content_editable_state(e) == CE_FALSE) return false;
        /* "either it is an HTML element, or it is an svg or math element" — an element in any other namespace
           is not editable and neither is anything below it, so the walk stops rather than skipping it. */
        if (!element_is_html(e) && !local_name_is(e, "svg") && !local_name_is(e, "math")) return false;
        p = parent_element(e);
        if (!p) return false;   /* "its parent is an editing host or editable" — a null parent is neither */
    }
    return false;
}

/* §6.8.1: "The contentEditable IDL attribute, on getting, must return the string `true` if the content
   attribute is set to the True state, `plaintext-only` if ... the Plaintext-Only state, `false` if ... the
   False state, and `inherit` otherwise." */
static JSValue js_content_editable_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    lxb_dom_element_t *el = receiver(ctx, this_val, "contentEditable");

    (void)magic;
    if (!el) return JS_EXCEPTION;
    switch (content_editable_state(el)) {
    case CE_TRUE:           return JS_NewString(ctx, "true");
    case CE_PLAINTEXT_ONLY: return JS_NewString(ctx, "plaintext-only");
    case CE_FALSE:          return JS_NewString(ctx, "false");
    default:                return JS_NewString(ctx, "inherit");
    }
}

/* §6.8.1's setter: an ASCII case-insensitive match for `inherit` REMOVES the attribute, `true`,
   `plaintext-only` and `false` set it to that exact lowercase string, and anything else throws a
   "SyntaxError" DOMException — which is a value a page can catch and therefore not an assert. */
static JSValue js_content_editable_set(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic)
{
    static const char *const KEEP[] = { "true", "plaintext-only", "false", NULL };
    lxb_dom_element_t *el = receiver(ctx, this_val, "contentEditable");
    const char *s;
    int i;

    (void)magic;
    if (!el) return JS_EXCEPTION;
    s = JS_ToCString(ctx, val);
    if (!s) return JS_EXCEPTION;
    if (enumerated_attribute_keyword_match("inherit", s, strlen(s))) {
        JS_FreeCString(ctx, s);
        dom_cow_remove_attribute(el, "contenteditable");
        return JS_UNDEFINED;
    }
    for (i = 0; KEEP[i]; i++) {
        if (!enumerated_attribute_keyword_match(KEEP[i], s, strlen(s))) continue;
        JS_FreeCString(ctx, s);
        element_attr_set(ctx, this_val, "contenteditable", KEEP[i]);
        return JS_UNDEFINED;
    }
    JS_FreeCString(ctx, s);
    return JS_ThrowDOMException(ctx, "SyntaxError",
                                "contentEditable must be set to \"true\", \"plaintext-only\", \"false\" or "
                                "\"inherit\"");
}

/* §6.8.1: "The isContentEditable IDL attribute, on getting, must return true if the element is either an
   editing host or editable, and false otherwise." */
static JSValue js_is_content_editable(JSContext *ctx, JSValueConst this_val, int magic)
{
    lxb_dom_element_t *el = receiver(ctx, this_val, "isContentEditable");

    (void)magic;
    if (!el) return JS_EXCEPTION;
    return JS_NewBool(ctx, is_editing_host(el) || is_editable(el));
}

/* ---- HTML §6.11.7 The draggable attribute ------------------------------------------------------------------ */

enum { DRAGGABLE_TRUE, DRAGGABLE_FALSE, DRAGGABLE_AUTO };
static const EnumeratedKeyword DRAGGABLE_KW[] = {
    { "true", DRAGGABLE_TRUE }, { "false", DRAGGABLE_FALSE }, { NULL, 0 }
};

/* §6.11.7: "The attribute's missing value default and invalid value default are both the Auto state." It
   declares NO empty value default, so `<div draggable="">` falls to the invalid one — which is what passing
   Auto in the `empty` position expresses, exactly as core/html/enumerated_attribute.h's own comment states. */
static int draggable_state(lxb_dom_element_t *el)
{
    return enumerated_attribute_state(el, "draggable", DRAGGABLE_KW,
                                      DRAGGABLE_AUTO, DRAGGABLE_AUTO, DRAGGABLE_AUTO);
}

/* §6.11.7's THREE-BRANCH getter, in the section's own order: "If an element's draggable content attribute has
   the state True, the draggable IDL attribute must return true. Otherwise, if ... the state False, ... return
   false. Otherwise, the element's draggable content attribute has the state Auto. If the element is an img
   element, an object element that represents an image, or an a element with an href content attribute, the
   draggable IDL attribute must return true; otherwise, ... false."
   THE `object` ARM IS UNREACHABLE IN THIS ENGINE AND THAT IS A FACT ABOUT §4.8.7, NOT A SHORTCUT HERE — the
   same shape as the `designMode` arm of is_editing_host above. "The object element represents the specified
   image" is a branch of §4.8.7's RUN THE OBJECT ELEMENT STEPS, reached only after that algorithm has FETCHED
   the resource and found a type starting with `image/` "and support for images has not been disabled". Nothing
   in this engine runs those steps and nothing decodes an image, so no object element has ever represented one;
   an element whose steps have not run represents its fallback content. Building §4.8.7's steps is what makes
   the arm reachable, and this is one of the readers it has to reach. */
static bool draggable_true(lxb_dom_element_t *el)
{
    switch (draggable_state(el)) {
    case DRAGGABLE_TRUE:  return true;
    case DRAGGABLE_FALSE: return false;
    default:              break;   /* the Auto state */
    }
    DCHECK(element_is_html(el),
           "§6.11.7's draggable was computed for an element that is not an HTML element — the member sits on "
           "HTMLElement.prototype and its receiver() brand check already refused everything else");
    if (local_name_is(el, "img")) return true;
    if (local_name_is(el, "a"))
        return lxb_dom_element_has_attribute(el, (const lxb_char_t *)"href", 4);
    return false;
}

static JSValue js_draggable_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    lxb_dom_element_t *el = receiver(ctx, this_val, "draggable");

    (void)magic;
    if (!el) return JS_EXCEPTION;
    return JS_NewBool(ctx, draggable_true(el));
}

/* §6.11.7: "If the draggable IDL attribute is set to the value false, the draggable content attribute must be
   set to the literal value `false`. If the draggable IDL attribute is set to the value true, the draggable
   content attribute must be set to the literal value `true`." Neither direction ever REMOVES the attribute,
   which is what separates this from a boolean reflection's setter. */
static JSValue js_draggable_set(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic)
{
    (void)magic;
    if (!receiver(ctx, this_val, "draggable")) return JS_EXCEPTION;
    element_attr_set(ctx, this_val, "draggable", JS_ToBool(ctx, val) ? "true" : "false");
    return JS_UNDEFINED;
}

/* ---- declaration and install ------------------------------------------------------------------------------ */

void global_attributes_declare(JSContext *ctx)
{
    DCHECK(!g_ready, "global_attributes_declare ran twice — the setters are declared once per AGENT");
    g_id_set_translate = idl_setter_id(ctx, IDL_BOOLEAN, false, js_translate_set, 0);
    g_id_set_spellcheck = idl_setter_id(ctx, IDL_BOOLEAN, false, js_spellcheck_set, 0);
    g_id_set_writing_suggestions = idl_setter_id(ctx, IDL_DOMSTRING, false, js_writing_suggestions_set, 0);
    g_id_set_autocapitalize = idl_setter_id(ctx, IDL_DOMSTRING, false, js_autocapitalize_set, 0);
    g_id_set_autocorrect = idl_setter_id(ctx, IDL_BOOLEAN, false, js_autocorrect_set, 0);
    g_id_set_content_editable = idl_setter_id(ctx, IDL_DOMSTRING, false, js_content_editable_set, 0);
    g_id_set_draggable = idl_setter_id(ctx, IDL_BOOLEAN, false, js_draggable_set, 0);
    g_ready = true;
}

void global_attributes_install(JSContext *ctx, JSValueConst proto)
{
    DCHECK(g_ready, "HTML's enumerated global attributes were installed before they were declared");
    idl_install_accessor(ctx, proto, "translate", js_translate_get, 0, g_id_set_translate);
    idl_install_accessor(ctx, proto, "spellcheck", js_spellcheck_get, 0, g_id_set_spellcheck);
    idl_install_accessor(ctx, proto, "writingSuggestions", js_writing_suggestions_get, 0,
                         g_id_set_writing_suggestions);
    idl_install_accessor(ctx, proto, "autocapitalize", js_autocapitalize_get, 0, g_id_set_autocapitalize);
    idl_install_accessor(ctx, proto, "autocorrect", js_autocorrect_get, 0, g_id_set_autocorrect);
    idl_install_accessor(ctx, proto, "contentEditable", js_content_editable_get, 0, g_id_set_content_editable);
    idl_install_accessor(ctx, proto, "isContentEditable", js_is_content_editable, 0, -1);
    idl_install_accessor(ctx, proto, "draggable", js_draggable_get, 0, g_id_set_draggable);
}

void global_attributes_free(void)
{
    g_id_set_translate = g_id_set_spellcheck = g_id_set_writing_suggestions = -1;
    g_id_set_autocapitalize = g_id_set_autocorrect = -1;
    g_id_set_content_editable = g_id_set_draggable = -1;
    g_ready = false;
}
