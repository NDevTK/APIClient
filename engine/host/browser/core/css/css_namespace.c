/* See css_namespace.h. CSSOM §8.1 The CSS.escape() Method's `namespace CSS`, plus CSS Conditional Rules
   Module Level 3 §7.5 The CSS namespace, and the supports() function's partial namespace — and CSS Typed OM
   Level 1 §4.3.5 Numeric Factory Functions' partial namespace, which is sixty-three of this object's members
   and ONE algorithm. */

#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"

#include "core/agent_state.h"
#include "core/css/css_namespace.h"
#include "core/css/css_serialize.h"
#include "core/css/css_style_declaration.h"
#include "core/css/css_supports.h"
#include "core/css/css_unit_value.h"
#include "core/idl_args.h"
#include "core/realm.h"

enum { M_ESCAPE, M_SUPPORTS, M_N };

static const char *const CSS_MEMBER[M_N] = { "escape", "supports" };

static int g_id[M_N] = { -1, -1 };

/* CSS TYPED OM 1 §4.3.5's SIXTY-THREE FACTORY FUNCTIONS, in the section's own order and with its own comments
 * marking where each unit family begins. It is a TABLE and not sixty-three bodies because §4.3.5 defines it as
 * one: "All of the above methods must, when called with a double value, return a new CSSUnitValue whose value
 * internal slot is set to value and whose unit internal slot is set to the name of the method as defined
 * here", and the section then says the naming is a shorthand "to avoid defining the unit individually for all
 * ~60 functions".
 *
 * THE UNIT IS THE TABLE'S ROW AND NEVER THE FUNCTION'S CURRENT NAME, which §4.3.5 warns about in a note of its
 * own: "The unit used does not depend on the current name of the function, if it's stored in another variable;
 * let foo = CSS.px; let val = foo(5); does not return a {value: 5, unit: "foo"} CSSUnitValue." A magic index
 * into this array is that sentence — the body never asks what it was called.
 *
 * THE SPELLINGS ARE THE SPEC'S, CASE INCLUDED. `Q`, `Hz` and `kHz` are written as css-values-4 §6.2 and §7.3
 * define them, which is what §4.3.5's own closing paragraph means by "named after the unit in its defined
 * canonical casing" — the METHOD NAME is a JavaScript property key and is matched exactly, even though the
 * unit identifier it denotes is matched ASCII case-insensitively everywhere a stylesheet carries one.
 *
 * THE LIST IS THE SPEC'S SET AND NOT THIS ENGINE'S, which is §4.3.5's own instruction and is why nothing here
 * consults the unit tables: the factories are defined as returning a CSSUnitValue whose unit slot is the
 * method's name, with no create-a-type step anywhere in the sentence — only §4.3.3's CONSTRUCTOR has one. So
 * a factory mints for a unit name whatever this engine's unit tables happen to hold, and only §4.3.3's
 * constructor asks. The six CSS Conditional 5 §7 Container Relative Lengths units used to WITNESS that
 * difference — `CSS.cqw(5)` answered while `new CSSUnitValue(5, "cqw")` threw — and they no longer do, because
 * core/css/css_length.h now carries them; the distinction stands because the standard states it, and the next
 * unit a specification defines before this engine's tables carry it is the next thing to show it. See
 * core/css/css_numeric_value.h for the create-a-type entry the constructor asks and the factories do not. */
#define CSS_UNIT_FN_N 63

static const char *const CSS_UNIT_FN[CSS_UNIT_FN_N] = {
    "number", "percent",
    /* <length> — css-values-4 §6.1.1's twelve font-relative units, §6.1.2's viewport-percentage family in all
       four of its spellings, CSS Conditional 5 §7's six container-relative units, and §6.2's seven absolute
       ones. */
    "cap", "ch", "em", "ex", "ic", "lh", "rcap", "rch", "rem", "rex", "ric", "rlh",
    "vw", "vh", "vi", "vb", "vmin", "vmax",
    "svw", "svh", "svi", "svb", "svmin", "svmax",
    "lvw", "lvh", "lvi", "lvb", "lvmin", "lvmax",
    "dvw", "dvh", "dvi", "dvb", "dvmin", "dvmax",
    "cqw", "cqh", "cqi", "cqb", "cqmin", "cqmax",
    "cm", "mm", "Q", "in", "pt", "pc", "px",
    /* <angle> — css-values-4 §7.1. */
    "deg", "grad", "rad", "turn",
    /* <time> — §7.2. */
    "s", "ms",
    /* <frequency> — §7.3. */
    "Hz", "kHz",
    /* <resolution> — §7.4. */
    "dpi", "dpcm", "dppx",
    /* <flex> — css-grid-2 §7.2.4. */
    "fr",
};

/* THE COUNT IS STATED AND THEN CHECKED AGAINST THE TABLE, because it is the array's declared width AND the
   bound of every loop below AND the range the factory body's magic is asserted against — three readings of one
   number, and a table edited without it would leave the tail of both arrays holding nothing. */
_Static_assert(sizeof CSS_UNIT_FN / sizeof CSS_UNIT_FN[0] == CSS_UNIT_FN_N,
               "CSS Typed OM 1 §4.3.5's factory table and the count declared for it disagree");

/* PRE-INITIALISED TO -1, WHICH IS core/agent_state.h's stated pre-init value for an id slot and is NOT what a
   static int array holds on its own: zero is a VALID pool entry, so a zeroed slot is indistinguishable from a
   declaration that really landed at entry 0. The rows mirror the table above one for one, so a reader checks
   the two shapes against each other by eye and the _Static_assert catches the rest. */
static int g_unit_fn_id[CSS_UNIT_FN_N] = {
    -1, -1,                                                            /* number, percent */
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,                    /* §6.1.1's twelve font-relative */
    -1, -1, -1, -1, -1, -1,                                            /* §6.1.2's `v*` */
    -1, -1, -1, -1, -1, -1,                                            /* `sv*` */
    -1, -1, -1, -1, -1, -1,                                            /* `lv*` */
    -1, -1, -1, -1, -1, -1,                                            /* `dv*` */
    -1, -1, -1, -1, -1, -1,                                            /* CSS Conditional 5 §7's `cq*` */
    -1, -1, -1, -1, -1, -1, -1,                                        /* §6.2's seven absolute */
    -1, -1, -1, -1,                                                    /* §7.1's angles */
    -1, -1,                                                            /* §7.2's times */
    -1, -1,                                                            /* §7.3's frequencies */
    -1, -1, -1,                                                        /* §7.4's resolutions */
    -1,                                                                /* css-grid-2 §7.2.4's `fr` */
};

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

/* ---- CSS Typed OM 1 §4.3.5's sixty-three factory functions, as ONE body ------------------------------------
 *
 * §4.3.5 in full, for all of them: "All of the above methods must, when called with a double value, return a
 * new CSSUnitValue whose value internal slot is set to value and whose unit internal slot is set to the name
 * of the method as defined here."
 *
 * THE ARGUMENT IS ALREADY A NUMBER OR IT IS UNKNOWN EXTERNAL INPUT, and this body takes NEITHER apart. Web IDL
 * §3.2's `double` conversion is the DECLARATION's — a page's own `valueOf` has already run, and a NaN or an
 * infinity has already been refused, because the IDL says `double` and not `unrestricted double`. What is left
 * is the value core/idl_args.h's boundary produced, which is exactly what §4.3.3's `value` internal slot is
 * defined to hold, so the slot is set to it as-is and opacity survives into `CSS.px(x).value`. */
static JSValue js_css_unit_factory(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                                   int magic)
{
    (void)this_val;
    DCHECK(argc == 1, "a §4.3.5 numeric factory reached its body with an argument count its IDL does not "
                      "declare — every one of the sixty-three declares exactly one required `value` position, "
                      "so the conversion machine owed this body exactly one argument");
    DCHECK(magic >= 0 && magic < CSS_UNIT_FN_N,
           "a §4.3.5 numeric factory was installed with a magic outside its table — the magic IS the unit, so "
           "one out of range would mint a value whose unit is whatever happens to follow the array");
    return css_unit_value_new(ctx, JS_DupValue(ctx, argv[0]), CSS_UNIT_FN[magic]);
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
    /* §4.3.5's partial namespace — the same step 3 over the same object, so the sixty-three land beside
       `escape` and `supports` with the identical descriptor and are indistinguishable from them to a page. */
    for (i = 0; i < CSS_UNIT_FN_N; i++)
        idl_install_method(ctx, ns, CSS_UNIT_FN[i], g_unit_fn_id[i]);
    /* Web IDL §3.13 Namespaces: "For every namespace that is exposed in a given realm, a corresponding
       property exists on the realm's global object." A namespace object is one of the five things §3.8's
       `define the global property references` reaches with DefineMethodProperty(target, id, x, false), so it
       takes §3.8's descriptor and this states it in the one place every interface name states it — the bits
       were spelled out by hand here and in console.c and NOWHERE ELSE, which is one right answer written twice
       beside the wrong one written eighty-odd times, and two hand-spellings are two places for the next one to
       drift. */
    idl_define_global_property_reference(ctx, global, "CSS", ns);
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
    /* Every one of CSS Typed OM 1 §4.3.5's sixty-three declares the identical `(double value)`, so they share
       ONE type list and differ only in the magic that names their unit. IDL_DOUBLE is Web IDL's RESTRICTED
       double — the IDL says `double` and not `unrestricted double` — so `CSS.px(NaN)` is a TypeError from the
       conversion and never a unit value carrying a number CSSOM §6.7.2 cannot serialize. */
    static const IdlArgType UNIT_FN_ARGS[1] = { IDL_DOUBLE };
    int i;

    DCHECK(g_id[M_ESCAPE] < 0 && g_id[M_SUPPORTS] < 0 && g_unit_fn_id[0] < 0,
           "css_namespace_init ran twice — this object's pool entries are the AGENT's and are declared once in "
           "it");

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

    /* §4.3.5's SIXTY-THREE, each `CSSUnitValue <unit>(double value)` — one required position, so §3.7.7's
       length is 1 for every one of them, and the MAGIC is the row, which is what makes the table the
       definition of the unit rather than the function's name (see CSS_UNIT_FN above). */
    for (i = 0; i < CSS_UNIT_FN_N; i++)
        g_unit_fn_id[i] = idl_method_id(ctx, UNIT_FN_ARGS, 1, js_css_unit_factory, i);

    for (i = 0; i < M_N; i++)
        DCHECK(g_id[i] >= 0, CSS_MEMBER[i]);
    for (i = 0; i < CSS_UNIT_FN_N; i++)
        DCHECK(g_unit_fn_id[i] >= 0, CSS_UNIT_FN[i]);
    for (i = 0; i < M_N; i++)
        agent_state_id("css_namespace", &g_id[i], "one of the CSS namespace's two operations");
    for (i = 0; i < CSS_UNIT_FN_N; i++)
        agent_state_id("css_namespace", &g_unit_fn_id[i],
                       "one of CSS Typed OM 1 §4.3.5's numeric factory functions");
    realm_declare_intrinsic(css_namespace_install_realm);
}

void css_namespace_free(void)
{
    /* EVERY HANDLE THIS ROW DECLARED, GIVEN BACK FROM THE ONE LIST THAT ALREADY NAMES THEM — the M_N
       operations and CSS Typed OM 1 §4.3.5's CSS_UNIT_FN_N numeric factory functions. This component holds
       nothing but pool ids, so there is no reference to give back and the undo is the whole release: the two
       loops that stood here walked the same two ranges the declarations walk, in a second place, and a range
       that drifted from its declaration's would have left the tail set with nothing to say so.
       LAST, AND THAT ORDER IS THE CONTRACT (core/agent_state.h) — it is trivially last here, and it is called
       BY this component rather than from core/platform.c's release column deliberately: a column that undid
       every component automatically would leave agent_state_check_released nothing to catch. */
    agent_state_undo("css_namespace");
}
