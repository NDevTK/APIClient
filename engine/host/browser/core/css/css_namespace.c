/* See css_namespace.h. CSSOM §8.1 The CSS.escape() Method's `namespace CSS`, plus CSS Conditional Rules
   Module Level 3 §7.5 The CSS namespace, and the supports() function's partial namespace. */

#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"

#include "core/agent_state.h"
#include "core/css/css_namespace.h"
#include "core/css/css_serialize.h"
#include "core/css/css_style_declaration.h"
#include "core/css/css_supports.h"
#include "core/idl_args.h"
#include "core/realm.h"

enum { M_ESCAPE, M_SUPPORTS, M_N };

static const char *const CSS_MEMBER[M_N] = { "escape", "supports" };

static int g_id[M_N] = { -1, -1 };

/* ---- CSSOM §8.1 The CSS.escape() Method ------------------------------------------------------------------
 *
 * "The escape(ident) operation must return the result of invoking serialize an identifier of ident." That is
 * the whole algorithm, and core/css/css_serialize.h holds the primitive it names — the same one CSSOM §6.4's
 * CSSNamespaceRule arm and §6.4.2's selector serialization go through, so `CSS.escape` cannot drift from what
 * the engine writes into a stylesheet.
 *
 * THE ARGUMENT IS A CSSOMString AND THIS BINDING MAKES THAT A DOMString — see core/css/css_serialize.h for the
 * CSSOM §3 CSSOMString choice, the reason this engine makes it, and the WPT assertion that forces it. It
 * matters HERE and nowhere else on this member: §8.1's own tests require a lone surrogate to come back
 * unchanged, so the declaration below must not be the type that replaces one. */
static JSValue js_css_escape(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    size_t len = 0;
    const char *ident;
    char *out;
    JSValue r;

    (void)this_val; (void)magic;
    DCHECK(argc == 1, "§8.1's escape() reached its body with an argument count its IDL does not declare — "
                      "`ident` is required and it is the only position, so the conversion machine owed this "
                      "body exactly one string");
    ident = JS_ToCStringLen(ctx, &len, argv[0]);
    /* NULL HERE IS ALLOCATION AND NOTHING ELSE, WHICH IS WHY IT IS A CHECK AND NOT A PROPAGATED THROW. The
       declared conversion already ran §7.1.19 ToString on this position, so the slot holds a real JSString: a
       page's own `toString` has already run (and would have thrown from the conversion, before this body), and
       UNKNOWN EXTERNAL INPUT never arrives — step_tostring_run answers a concolic with JS_STEP_UNKNOWN and
       collapses the machine ahead of the body. What is left is the encoder's own buffer. */
    CHECK(ident != NULL, "cssom: OOM encoding the argument of §8.1's escape()");
    out = css_serialize_identifier(ident, len);
    JS_FreeCString(ctx, ident);
    DCHECK(out != NULL, "§2.1's serialize an identifier answered nothing at all — it returns an owned string "
                        "for every input including the empty one, so a null is an allocation that was not "
                        "checked rather than an identifier that has no serialization");
    r = JS_NewStringLen(ctx, out, strlen(out));
    free(out);
    return r;
}

/* ---- CSS Conditional Rules 3 §7.5's supports(), the ONE-ARGUMENT overload ---------------------------------
 *
 * §7.5's one-argument list, in full: "If conditionText, parsed and evaluated as a <supports-condition>, would
 * return true, return true. Otherwise, If conditionText, wrapped in parentheses and then parsed and evaluated
 * as a <supports-condition>, would return true, return true. Otherwise, return false."
 *
 * THE SECOND STEP IS WHY `CSS.supports("display:flex")` IS TRUE AT ALL. A bare declaration is not a
 * `<supports-condition>` — §6's grammar has no arm that begins with an IDENT — so the first step answers
 * false, and the retry is what turns the argument into the `(display:flex)` that §6.1's definition of support
 * is stated over. WPT names that requirement in its own words ("CSS.supports(arg1) implies parentheses").
 *
 * "WOULD RETURN TRUE" IS ONE BOOLEAN AND core/css/css_supports.h ANSWERS WITH TWO, which is not a mismatch:
 * that entry separates GRAMMAR-VALID from TRUE because a `@supports` at-rule needs the difference (an invalid
 * prelude drops the rule and its contents; a false one keeps them). §7.5 needs only the conjunction — a text
 * that matches no production did not "return true" — so both steps read the two together, and a `false`
 * return with `matches` untouched is exactly the case the retry exists for. */
static bool css_supports_condition_text(const char *text, size_t len)
{
    bool matches = false;
    char *wrapped;
    bool r;

    DCHECK(text != NULL, "§7.5's supports() was asked about no condition text at all — the empty string is a "
                         "real argument that matches no production of §6's grammar, and the absence of one is "
                         "a caller that never took the argument");
    if (css_supports_condition(text, len, &matches) && matches)
        return true;
    /* "wrapped in parentheses and then parsed and evaluated as a <supports-condition>" — the argument's own
       bytes with one character on each side, never a re-serialization of it: §7.5 says NOTHING about
       normalising the text, and a member that trimmed or re-encoded it would answer a different question from
       the one the page asked. */
    wrapped = malloc(len + 3);
    CHECK(wrapped != NULL, "cssom: OOM wrapping §7.5's condition text in parentheses");
    wrapped[0] = '(';
    memcpy(wrapped + 1, text, len);
    wrapped[len + 1] = ')';
    wrapped[len + 2] = '\0';
    matches = false;
    r = css_supports_condition(wrapped, len + 2, &matches) && matches;
    free(wrapped);
    return r;
}

/* ---- CSS Conditional Rules 3 §7.5's supports(), the TWO-ARGUMENT overload ---------------------------------
 *
 * §7.5's two-argument list, in full: "If property is an ASCII case-insensitive match for any defined CSS
 * property that the UA supports, or is a custom property name string, and value successfully parses according
 * to that property's grammar, return true. Otherwise, return false."
 *
 * IT IS NOT §6.1's DEFINITION OF SUPPORT WITH A COLON IN IT, and §7.5's own two Notes are the refutation. The
 * reading is written out here rather than merely refused, because it is the obvious one and a reader who
 * re-derives it will re-introduce it:
 *   - Note: "No CSS escape or whitespace processing is performed on the property name, so CSS.supports("
 *     width", "5px") will return false, as " width" isn't the name of any property due to the leading space."
 *     A colon-join makes that ` width: 5px`, whose leading whitespace the tokenizer consumes before the
 *     property name, so §6.1 answers TRUE where §7.5 requires FALSE.
 *   - Note: "!important flags are not part of property grammars, and will cause value to parse as invalid,
 *     just as they would in the value argument to element.style.setProperty()." A colon-join makes that
 *     `width: 5px !important`, which IS one declaration §6.1 supports, so again TRUE for a required FALSE.
 * The first Note is what makes this TWO questions rather than one parse: the property name is matched against
 * a SET, on its literal bytes, before any parser sees it.
 *
 * THE FIRST HALF IS CSSOM §2 Terminology's SUPPORTED CSS PROPERTY SET, asked through the same entry §6.6.1's
 * per-property IDL attributes are built from (core/css/css_style_declaration.h's
 * `cssom_supported_css_property_named`), so `CSS.supports("width","5px")` and the existence of
 * `el.style.width` are ONE answer. Two entries deciding what this engine supports could disagree, and the
 * disagreement would read as a page bug.
 *
 * THE SECOND HALF IS CSSOM §6.7.1 Parsing CSS Values, asked through the entry `setProperty` already uses
 * (`cssom_parse_a_css_value`), which is where §6.7.1's own Note about `!important` is obeyed — so the second
 * of §7.5's Notes is satisfied by the component that owns the rule rather than by a check written again here.
 *
 * THE NAME HANDED TO THE PARSE IS THE REGISTRY'S, NOT THE PAGE'S, and that is what makes the ASCII
 * case-insensitive match real. `cssom_parse_a_css_value` requires the declaration it parses to come back
 * under the name it was given, so handing it `WIDTH` would answer false for a property §7.5 says is
 * supported; the lookup returns the canonical spelling and the parse is asked about that. A CUSTOM PROPERTY
 * has no registry row and no canonical spelling other than its own bytes, which is why its arm passes the
 * argument through. */
static bool css_supports_property_value(const char *property, const char *value)
{
    const char *canonical;
    char *parsed;

    DCHECK(property != NULL && value != NULL,
           "§7.5's two-argument supports() was asked with no property name or no value — both positions are "
           "required in the entry that survives at this arity, so a missing one is a conversion that did not "
           "run rather than an argument the page omitted");
    canonical = cssom_supported_css_property_named(property);
    /* CSS Typed OM Level 1 §3 The StylePropertyMap: "A string is a custom property name string if it starts
       with two dashes (U+002D HYPHEN-MINUS), like --foo." §7.5 names that term, and Typed OM's own parenthesis
       says why it is a STRING test and not a parse: "it can be used without invoking the CSS parser". A
       two-dash prefix is therefore the whole of this arm — a custom property is excluded from CSSOM §2's
       supported-property set by name, so the lookup above could never have answered for one. */
    if (canonical == NULL && property[0] == '-' && property[1] == '-')
        canonical = property;
    if (canonical == NULL)
        return false;
    parsed = cssom_parse_a_css_value(canonical, value);
    if (parsed == NULL)
        return false;
    free(parsed);
    return true;
}

/* ONE BODY FOR BOTH OVERLOADS, DISPATCHED ON THE ARGUMENT COUNT AND ON NOTHING ELSE. Web IDL §3.6 Overload
   resolution algorithm's effective overload set for `supports` holds one entry of length 1 and one of length
   2, both with the same type at every position, so steps 3-4 remove one of them by COUNT alone and there is no
   distinguishing argument index to compute. `argc` here is that count: the conversion machine hands a
   non-variadic body min(passed, declared), and the declaration below states the split so that at count 2 the
   surviving entry's optionality is read — which is what makes `CSS.supports("(width:1px)", undefined)` the
   two-argument call it is, with `undefined` converted to the string, rather than a one-argument call whose
   condition would be true. */
static JSValue js_css_supports(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    size_t len = 0;
    const char *a, *b;
    bool r;

    (void)this_val; (void)magic;
    DCHECK(argc == 1 || argc == 2,
           "§7.5's supports() reached its body at an arity neither of its overload entries has — Web IDL "
           "§3.6 step 5 throws before any conversion for a call shorter than the surviving entry requires, and "
           "the member declares two positions, so a count outside 1..2 is a declaration that does not match "
           "this body");
    /* Both encodes are allocation-only failures for the reason js_css_escape states above. */
    a = JS_ToCStringLen(ctx, &len, argv[0]);
    CHECK(a != NULL, "cssom: OOM encoding the first argument of §7.5's supports()");
    if (argc == 1) {
        r = css_supports_condition_text(a, len);
        JS_FreeCString(ctx, a);
        return JS_NewBool(ctx, r);
    }
    b = JS_ToCStringLen(ctx, NULL, argv[1]);
    CHECK(b != NULL, "cssom: OOM encoding the value argument of §7.5's supports()");
    r = css_supports_property_value(a, b);
    JS_FreeCString(ctx, a);
    JS_FreeCString(ctx, b);
    return JS_NewBool(ctx, r);
}

/* ---- the per-realm install ------------------------------------------------------------------------------- */

static void css_namespace_install_realm(JSContext *ctx)
{
    JSValue ns, global;
    int i;

    global = JS_GetGlobalObject(ctx);
    /* Web IDL §3.13.1 Namespace object step 1 makes a namespace object an ordinary object whose [[Prototype]]
       is %Object.prototype% — CSSOM states no departure from that, so unlike Console §1's interposed empty
       object this is the plain one. */
    ns = JS_NewObject(ctx);
    CHECK(!JS_IsException(ns), "cssom: the CSS namespace object could not be allocated");
    /* Web IDL §3.13.1's last line: "The class string of a namespace object is the namespace's identifier." So
       `Object.prototype.toString.call(CSS)` is "[object CSS]", which CSSOM §8.1's own linked test
       (CSS-namespace-object-class-string.html) is entirely about. It is written BEFORE the operations because
       §3 JavaScript binding says an object with a class string carries it at the time it is created, and
       because engine/idl_installed.mjs reads this statement to decide whose definition the properties below
       belong to. */
    idl_namespace_tag(ctx, ns, "CSS");
    /* Web IDL §3.13.1 step 3 "Define the regular operations of namespace on namespaceObject", whose descriptor
       §3.7.7 Operations states as { [[Writable]]: true, [[Enumerable]]: true, [[Configurable]]: true }. The
       `length` of each is §3.7.7's own derivation and is the pool's to compute, not this install's. */
    for (i = 0; i < M_N; i++)
        idl_install_method(ctx, ns, CSS_MEMBER[i], g_id[i]);
    /* Web IDL §3.13 Namespaces: "For every namespace that is exposed in a given realm, a corresponding
       property exists on the realm's global object." Its own step is DefineMethodProperty(target, id,
       namespaceObject, false) — writable, NOT enumerable, configurable — which JS_SetPropertyStr is not: that
       would make `CSS` turn up in a page's `for (const k in window)`. */
    CHECK(JS_DefinePropertyValueStr(ctx, global, "CSS", ns,
                                    JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE) >= 0,
          "cssom: the CSS namespace object could not be defined on the global");
    JS_FreeValue(ctx, global);
}

void css_namespace_init(JSContext *ctx)
{
    /* `CSSOMString escape(CSSOMString ident)` — one required argument, so §3.7.7's length is 1 and
       `CSS.escape()` is a TypeError, both of which the pool derives from this declaration alone. */
    static const IdlArgType ESCAPE_ARGS[1] = { IDL_DOMSTRING };
    /* `boolean supports(CSSOMString property, CSSOMString value)` and
       `boolean supports(CSSOMString conditionText)` — ONE declaration carrying both of §3.6's entries, since
       they share their type at every position and differ only in LENGTH. */
    static const IdlArgType SUPPORTS_ARGS[2] = { IDL_DOMSTRING, IDL_DOMSTRING };
    int i;

    DCHECK(g_id[M_ESCAPE] < 0 && g_id[M_SUPPORTS] < 0,
           "css_namespace_init ran twice — the two pool entries are the AGENT's and are declared once in it");

    g_id[M_ESCAPE] = idl_method_id(ctx, ESCAPE_ARGS, 1, js_css_escape, M_ESCAPE);

    g_id[M_SUPPORTS] = idl_method_id(ctx, SUPPORTS_ARGS, 2, js_css_supports, M_SUPPORTS);
    /* THE SHORTER ENTRY'S OWN OPTIONALITY: `supports(conditionText)` declares one required position and ends
       there, so its "there are none" value is 1. This is also what §3.7.7's length is read off at argument
       count 0, and 1 is the number CSSOM's sibling test asserts for `escape`; WPT asserts the same shape here
       through `CSS.supports()` throwing. */
    idl_optional_from(1);
    /* §3.6's LENGTH-DIFFERING SPLIT, BY ARITY: the shorter entry's type list ends at position 0. */
    idl_overload_length_split_at(0);
    /* THE LONGER ENTRY'S OWN OPTIONALITY: `supports(property, value)` declares TWO required positions, so its
       "there are none" value is 2 — which is what makes position 1 a CONVERTED argument at argument count 2
       instead of §3.6 step 15.4.2's "missing". */
    idl_overload_split_optional_from(2);

    for (i = 0; i < M_N; i++)
        DCHECK(g_id[i] >= 0, CSS_MEMBER[i]);
    for (i = 0; i < M_N; i++)
        agent_state_id("css_namespace", &g_id[i], "one of the CSS namespace's two operations");
    realm_declare_intrinsic(css_namespace_install_realm);
}

void css_namespace_free(void)
{
    int i;

    for (i = 0; i < M_N; i++) g_id[i] = -1;
}
