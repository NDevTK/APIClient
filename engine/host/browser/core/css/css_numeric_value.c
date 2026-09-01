/* See css_numeric_value.h. CSS Typed OM 1 §4.3.1 Common Numeric Operations, and the CSSNumericValue
   Superclass, over §4.3.2 Numeric Value Typing. The three interface prototype objects this file's members land
   on are core/css/css_unit_value.c's, for the Web IDL §3.7.3 Interface prototype object reason that file
   states. */

#include <ctype.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"

#include "core/agent_state.h"
#include "core/css/css_dimension.h"
#include "core/css/css_length.h"
#include "core/css/css_math.h"
#include "core/css/css_numeric_value.h"
#include "core/css/css_unit_value.h"
#include "core/idl_args.h"
#include "solver/concolic.h"

static int g_id[CSS_NUMERIC_MEMBER_N] = { -1, -1 };

/* ---- §4.3.2 Numeric Value Typing's "create a type from a string unit" -------------------------------------- */

/* §4.3.2's two LITERAL branches — "unit is "number"" and "unit is "percent"" — compared as the section spells
   them, by code points and not case-insensitively. That is the spec's own shape rather than a shortcut: the
   other seven branches are stated over unit PRODUCTIONS ("unit is a <length> unit") and CSS matches unit
   IDENTIFIERS ASCII case-insensitively, but neither "number" nor "percent" is a unit identifier at all — no
   CSS dimension token carries them, and §4.3.5 Numeric Factory Functions mints them from method names it fixes
   itself. */
static bool nv_literal_is(const char *unit, size_t unit_len, const char *lit)
{
    size_t n = strlen(lit);

    return unit_len == n && memcmp(unit, lit, n) == 0;
}

bool css_numeric_type_from_unit(const char *unit, size_t unit_len, CssMathType *out)
{
    CssMathBase base;

    DCHECK(unit != NULL || unit_len == 0,
           "§4.3.2's create-a-type was asked about a NULL span with a non-zero length — every caller holds the "
           "bytes it is asking about, so an absent pointer is a caller that lost the buffer");
    DCHECK(out != NULL,
           "§4.3.2's create-a-type was handed nowhere to write the type. The map IS the answer for §4.3.1's "
           "type() and the failure half alone is what §4.3.3's constructor reads, so a caller with no "
           "destination is one that wanted a predicate this entry deliberately does not offer");
    /* "unit is "number" → Return «[ ]» (empty map)". */
    if (nv_literal_is(unit, unit_len, "number")) { *out = css_math_type_number(); return true; }
    /* "unit is "percent" → Return «[ "percent" → 1 ]»". */
    if (nv_literal_is(unit, unit_len, "percent")) { *out = css_math_type_of(CSS_MATH_PERCENT); return true; }
    /* §4.3.2's six PRODUCTION branches — <length>, <angle>, <time>, <frequency>, <resolution>, <flex> — which
       are css-values-4 §10.9 Type Checking's terminal rule for a dimension over the same unit tables. Each
       returns «[ <that base type> → 1 ]». */
    if (!css_math_unit_base(unit, unit_len, &base)) return false;   /* "anything else → Return failure" */
    *out = css_math_type_of(base);
    /* "In all cases, the associated percent hint is null." — which is what both constructors above give, and
       is asserted here rather than trusted because a hint that arrived from a unit would make `CSS.px(1)`'s
       type match `<percentage>` in a context that resolves percentages against a length. */
    DCHECK(out->hint == CSS_MATH_HINT_NULL,
           "§4.3.2's create-a-type-from-a-string-unit produced a type with a PERCENT HINT. That section's last "
           "sentence is \"In all cases, the associated percent hint is null\" — a hint is applied by §4.3.2's "
           "add and multiply and by nothing else — so one here is core/css/css_math.c's type constructors "
           "having stopped starting from a null hint");
    return true;
}

/* ---- §4.3.1's brand, and the one subclass this engine can answer for --------------------------------------- */

/* Web IDL §3.7.5 Operations' brand check: a member of CSSNumericValue.prototype reached on something that is
 * not a CSSNumericValue is a TypeError, and a page tells that apart from `undefined`.
 *
 * NAMED RESIDUAL — THE BRAND ASKS FOR THE ONE SUBCLASS THAT EXISTS, NOT FOR THE INTERFACE.
 * WHAT IS NOT COVERED: `css_unit_value_is` answers "is this a §4.3.3 CSSUnitValue", and the question Web IDL
 * asks here is "is this a §4.3.1 CSSNumericValue". They coincide over every object that exists, because
 * §4.3.4 Complex Numeric Values: CSSMathValue objects is unbuilt and CSSUnitValue is the only subclass any
 * component mints — so this is correct today and narrower than the interface.
 * WHAT THE NEXT DIFF BUILDS: the CSSMathValue family, whose first member brings a second brand; this predicate
 * becomes the disjunction of the two at that moment, and the members below grow the CSSMathValue arms of
 * §4.3.1's create-a-sum-value that they DCHECK against today.
 * HOW ITS ABSENCE WOULD SHOW: it cannot show as a wrong answer while it holds — there is no other object for
 * it to be wrong about — which is why it is stated rather than asserted. It shows the day a CSSMathValue is
 * minted and `mathValue.type()` throws a TypeError instead of answering. */
static bool nv_brand(JSContext *ctx, JSValueConst v)
{
    if (css_unit_value_is(v)) return true;
    JS_ThrowTypeError(ctx, "a CSSNumericValue member was reached on something that is not a CSSNumericValue");
    return false;
}

/* ---- css-values-4 §5.4.1 Compatible Units, which is the whole of what `to()` needs -------------------------- */

/* §5.4.1: "compatible units (those related by a static multiplicative factor, like the 96:1 factor between px
 * and in, or the computed font-size factor between em and px) are converted into a single canonical unit. Each
 * group of compatible units defines which among them is the canonical unit."
 *
 * THIS ENGINE ANSWERS THE STATIC HALF AND MUST NOT ANSWER THE OTHER, and that is the spec's own line rather
 * than a gap. The parenthetical's second example — the em-to-px factor — is a COMPUTED font size, and §5.4.1
 * is stated "when serializing computed values", where an element is in hand. §4.3.1's `to()` has no element:
 * a CSSUnitValue carries a unit and resolves nothing. So the sets this entry knows are the five whose factor
 * is fixed by a specification — css-values-4 §6.2's seven absolute lengths (canonical `px`), §7.1's four
 * angles (`deg`), §7.2's two times (`s`), §7.3's two frequencies (`hz`) and §7.4's resolutions (`dppx`) — and
 * a font-relative, viewport-percentage or container-relative length is a set of ONE, which is why
 * `CSS.em(1).to("px")` is the TypeError §4.3.1 step 6 gives and `CSS.em(1).to("em")` is 1em.
 *
 * THE TABLES ARE THE COMPONENTS' OWN. Each row below is one existing entry asked for the number of canonical
 * units ONE of `unit` is worth; a sixth table here would be the copy that disagrees about `dpcm` the day one
 * of them is edited. Writes nothing and answers false for a unit in no set at all — "number", "percent",
 * `em`, `cqw`, `fr` — which is a POSITIVE statement (this unit is compatible with itself alone) and not an
 * absence the caller has to default past. */
static bool nv_canonical(const char *unit, size_t unit_len, double *per, const char **canon)
{
    double v = 0.0;

    if (css_length_absolute_px(unit, unit_len, 1.0, &v))  { *per = v; *canon = "px";   return true; }
    if (css_angle_deg(unit, unit_len, 1.0, &v))           { *per = v; *canon = "deg";  return true; }
    if (css_time_s(unit, unit_len, 1.0, &v))              { *per = v; *canon = "s";    return true; }
    if (css_frequency_hz(unit, unit_len, 1.0, &v))        { *per = v; *canon = "hz";   return true; }
    if (css_resolution_dppx(unit, unit_len, 1.0, &v))     { *per = v; *canon = "dppx"; return true; }
    return false;
}

/* Are two unit identifiers the SAME unit — the singleton case of §5.4.1, where the static multiplicative
   factor relating a unit to itself is 1. ASCII case-insensitive, because CSS matches unit identifiers that
   way; §4.3.2's two literal branches never reach here under a case variant, since "NUMBER" has no type at all
   and `to()` has already thrown for it. */
static bool nv_unit_same(const char *a, const char *b)
{
    size_t i;

    for (i = 0; a[i] != '\0' && b[i] != '\0'; i++)
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return false;
    return a[i] == b[i];
}

/* See css_numeric_value.h.
 *
 * WHY THE TWO STEPS COLLAPSE INTO A RATIO AND NOTHING IS LOST. The sum-value step multiplies the value by the
 * factor from `from` to its canonical unit; the conversion step multiplies by the factor from that canonical
 * unit to `want`. Composed, that is `per(from) / per(want)` — and the intermediate never has to be materialized
 * because a CSSUnitValue's sum value is one entry with one unit at power 1, which is exactly the shape
 * §4.3.1's create-a-CSSUnitValue-from-a-sum-value-item hands straight back.
 *
 * THE THREE WAYS IT REFUSES are §4.3.3's one sentence read over the sets: one unit in a set and the other not,
 * two units in DIFFERENT sets, or two units in no set that are not the same unit. */
bool css_numeric_convert_ratio(const char *from, const char *want, double *ratio)
{
    const char *from_canon = NULL, *want_canon = NULL;
    double per_from = 1.0, per_want = 1.0;
    bool from_set, want_set;

    from_set = nv_canonical(from, strlen(from), &per_from, &from_canon);
    want_set = nv_canonical(want, strlen(want), &per_want, &want_canon);
    if (from_set != want_set) return false;
    if (!from_set) {
        if (!nv_unit_same(from, want)) return false;
        *ratio = 1.0;
        return true;
    }
    if (strcmp(from_canon, want_canon) != 0) return false;
    DCHECK(per_want != 0.0,
           "a css-values-4 §5.4.1 compatible unit is worth ZERO of its own set's canonical unit. Every ratio "
           "the five tables state is a positive constant of the specification that defines the family, so a "
           "zero is a table row that lost its factor and is about to divide by it");
    *ratio = per_from / per_want;
    return true;
}

/* ---- §4.3.1's `type()` ------------------------------------------------------------------------------------- */

/* §4.3.2's SEVEN BASE TYPES, in that section's own order — which is `CssMathBase`'s order because core/css/
   css_math.h built that enum from this sentence: "The base types are "length", "angle", "time", "frequency",
   "resolution", "flex", and "percent"." The strings are the `CSSNumericBaseType` enumeration's seven values,
   which is what §4.3.1's `percentHint` member carries. */
static const char *const NV_BASE_NAME[CSS_MATH_BASE_COUNT] = {
    "length", "angle", "time", "frequency", "resolution", "flex", "percent",
};

#define NV_DICT_HINT (-1)

/* THE `CSSNumericType` DICTIONARY'S MEMBERS IN THE ORDER THE PROPERTIES ARE CREATED, WHICH IS NOT §4.3.2's
   ORDER AND IS OBSERVABLE. Web IDL §3.2.17 Dictionary types converts an IDL dictionary to a JS object by
   "For each dictionary member member declared on dictionary, in lexicographical order: … Perform !
   CreateDataPropertyOrThrow(O, key, value)" — so `Object.keys()` of the result is the member IDENTIFIERS
   sorted, while §4.3.2's own sentence ("The ordering of a type's entries always matches this base type
   ordering") orders the TYPE's entries and stops there. The two disagree: a type carrying both `length` and
   `angle` enumerates angle-then-length here and length-then-angle in §4.3.2's map. `CSSNumericType` inherits
   from no dictionary, so §3.2.17's outer loop over inherited dictionaries has one entry and this is the whole
   of it. */
static const struct { const char *name; int base; } NV_DICT[] = {
    { "angle",       CSS_MATH_ANGLE      },
    { "flex",        CSS_MATH_FLEX       },
    { "frequency",   CSS_MATH_FREQUENCY  },
    { "length",      CSS_MATH_LENGTH     },
    { "percent",     CSS_MATH_PERCENT    },
    { "percentHint", NV_DICT_HINT        },
    { "resolution",  CSS_MATH_RESOLUTION },
    { "time",        CSS_MATH_TIME       },
};

/* The dictionary declares one `long` per base type plus the hint, so a row added to `CssMathBase` without a
   row here would silently stop being reported by `type()` — which is a member answering a type that is missing
   an exponent it has. */
_Static_assert(sizeof NV_DICT / sizeof NV_DICT[0] == CSS_MATH_BASE_COUNT + 1,
               "CSS Typed OM 1 §4.3.1's CSSNumericType members and §4.3.2's base types have come apart");

/* "The type() method returns a representation of the type of this. When called, it must perform the following
   steps: Let result be a new CSSNumericType. For each baseType → power in the type of this, If power is not 0,
   set result[baseType] to power. If the percent hint of this is not null, Set result[percentHint] to the
   percent hint of this. Return result." */
static JSValue js_css_numeric_value_type(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                                         int magic)
{
    CssMathType t;
    const char *unit;
    JSValue result;
    unsigned i;

    (void)argc; (void)argv; (void)magic;
    if (!nv_brand(ctx, this_val)) return JS_EXCEPTION;
    unit = css_unit_value_unit(this_val);
    /* §4.3.3: "The type of a CSSUnitValue is the result of creating a type from its unit internal slot." */
    if (!css_numeric_type_from_unit(unit, strlen(unit), &t)) {
        DFAIL("a CSSUnitValue carries a unit that §4.3.2's create-a-type-from-a-string-unit REFUSES, so §4.3.3 "
              "gives it no type at all and `type()` has nothing to answer. Only two things mint a "
              "CSSUnitValue: §4.3.3's constructor, whose step 1 throws a TypeError for exactly this, and "
              "§4.3.5's sixty-three factory functions, which carry no create-a-type step and take their unit "
              "from the method name — so this is core/css/css_namespace.c's factory table naming a unit that "
              "core/css/css_length.h's and core/css/css_dimension.h's tables do not, and the missing unit "
              "family is the component to BUILD");
        return JS_UNDEFINED;
    }
    /* §3.2.17's step 1: "Let O be OrdinaryObjectCreate(%Object.prototype%)" — which is a plain object of THIS
       realm, and JS_NewObject is exactly that. */
    result = JS_NewObject(ctx);
    CHECK(!JS_IsException(result), "css-typed-om: a CSSNumericType could not be allocated");
    for (i = 0; i < sizeof NV_DICT / sizeof NV_DICT[0]; i++) {
        JSValue v;

        if (NV_DICT[i].base == NV_DICT_HINT) {
            if (t.hint == CSS_MATH_HINT_NULL) continue;   /* "If the percent hint of this is not null" */
            DCHECK((unsigned)t.hint < (unsigned)CSS_MATH_BASE_COUNT,
                   "a CSS numeric type carries a percent hint that is neither null nor one of §4.3.2's seven "
                   "base types — that section states \"The percent hint is either null or a base type\", so a "
                   "third thing is a hint that was applied from outside the two constructors that can make one");
            v = JS_NewString(ctx, NV_BASE_NAME[t.hint]);
        }
        else {
            if (t.exp[NV_DICT[i].base] == 0) continue;    /* "If power is not 0" */
            /* `long` is Web IDL §3.2's 32-bit signed integer, which is what a type's exponent is: css-values-4
               §10.9's own worked example is `calc(1px * 1em)` at «["length" → 2]». */
            v = JS_NewInt32(ctx, t.exp[NV_DICT[i].base]);
        }
        /* §3.2.17's "Perform ! CreateDataPropertyOrThrow(O, key, value)" — an ordinary data property, which is
           writable, enumerable and configurable. A page reads these back with `Object.keys`, so the attributes
           are the conversion's and not this component's choice. */
        CHECK(JS_DefinePropertyValueStr(ctx, result, NV_DICT[i].name, v, JS_PROP_C_W_E) >= 0,
              "css-typed-om: a CSSNumericType member could not be defined");
    }
    return result;
}

/* ---- §4.3.1's `to()` --------------------------------------------------------------------------------------- */

/* "The to(unit) method converts an existing CSSNumericValue this into another one with the specified unit, if
   possible. When called, it must perform the following steps: Let type be the result of creating a type from
   unit. If type is failure, throw a SyntaxError. Let sum be the result of creating a sum value from this. If
   sum is failure, throw a TypeError. If sum has more than one item, throw a TypeError. Otherwise, let item be
   the result of creating a CSSUnitValue from the sole item in sum, then converting it to unit. If item is
   failure, throw a TypeError. Return item."
   THE UNIT'S BYTES COME THROUGH `concolic_name_cstr`, exactly as §4.3.3's constructor's do: Web IDL's boundary
   passes unknown external input across as itself, and an unknown denotes its SHAPE — a real string, stable per
   source — which is not one of §4.3.2's branches, so `CSS.px(1).to(location.hash)` throws the SyntaxError the
   standard gives for a unit that is not a unit. */
static JSValue js_css_numeric_value_to(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                                       int magic)
{
    CssMathType want_type;
    const char *want, *from;
    double ratio = 1.0, n = 0.0;
    JSValue slot, value, example, r;

    (void)magic;
    DCHECK(argc == 1, "§4.3.1's to() reached its body with an argument count its IDL does not declare — "
                      "`unit` is its one required position and there are no others, so the conversion machine "
                      "owed this body exactly one argument");
    if (!nv_brand(ctx, this_val)) return JS_EXCEPTION;
    want = concolic_name_cstr(ctx, argv[0]);
    CHECK(want != NULL, "css-typed-om: OOM encoding the `unit` argument of §4.3.1's to()");
    /* STEPS 1-2. The type itself is not read: `to()` uses it only for the failure test, because the unit the
       result carries is the ARGUMENT and not a canonical spelling derived from its type. */
    if (!css_numeric_type_from_unit(want, strlen(want), &want_type)) {
        r = JS_ThrowDOMException(ctx, "SyntaxError", "'%s' is not a CSS unit a CSSNumericValue can convert to",
                                 want);
        JS_FreeCString(ctx, want);
        return r;
    }
    /* STEPS 3-5 over the one subclass that exists. §4.3.1's create-a-sum-value has a CSSUnitValue arm that
       cannot fail and yields exactly one item, so the two TypeErrors those steps guard against are unreachable
       from here — see nv_brand's residual for the arm that makes them reachable. What is left is §4.3.3's
       convert-a-CSSUnitValue, whose failure IS step 6's TypeError. */
    from = css_unit_value_unit(this_val);
    if (!css_numeric_convert_ratio(from, want, &ratio)) {
        r = JS_ThrowTypeError(ctx, "a CSSUnitValue in '%s' cannot be converted to '%s' — css-values-4 §5.4.1 "
                                   "relates compatible units by a static factor and these two are in "
                                   "different sets", from, want);
        JS_FreeCString(ctx, want);
        return r;
    }
    DCHECK(ratio == ratio && ratio != 0.0,
           "css-values-4 §5.4.1's conversion ratio between two COMPATIBLE units is not a usable factor. Both "
           "operands were found in one set whose ratios are positive constants of the specification that "
           "defines the family, so a NaN or a zero is a quotient of two table rows one of which is absent");
    slot = css_unit_value_value(ctx, this_val);
    /* §4.3.3's "old value multiplied by the conversion ratio between old unit and unit". A ratio of exactly 1
       IS that multiplication for every value the slot can hold — Web IDL §3.2's RESTRICTED `double` refused a
       NaN at the boundary — so `CSS.px(x).to("px")` hands back the value the page put in rather than a second
       object that merely equals it, which is what `x * 1` evaluates to and what keeps one unknown one unknown
       instead of two that have to be compared. */
    if (ratio == 1.0)
        value = slot;
    else {
        /* `idl_number_of` is what a body reads a converted numeric slot through: for a Number it is the
           number, and for unknown external input it is §3.2's conversion RUN ON THAT VALUE'S OWN EXAMPLE —
           the concrete the page actually computed — answering false when there is no example yet. */
        example = idl_number_of(ctx, IDL_DOUBLE, slot, &n) ? JS_NewFloat64(ctx, n * ratio) : JS_UNDEFINED;
        if (!concolic_is(slot)) {
            DCHECK(!JS_IsUndefined(example),
                   "§4.3.3's conversion produced no number for a `value` slot this engine knows the number of "
                   "— a Number always has an example by definition, so this is idl_number_of's two answers "
                   "having come apart");
            value = example;
        }
        else {
            /* THE VALUE IS UNKNOWN EXTERNAL INPUT, SO THE CONVERSION OF IT IS TOO. A concrete number here
               would DE-TAINT what `CSS.px(attackerNumber).to("in")` carries into whatever consumes it, which
               is exactly the placeholder solver/concolic.h's builtin seam exists instead of. The derived value
               keeps the operand's source and root and carries the REAL multiplication on its example as its
               own — never a rule predicting what the conversion would have produced. */
            value = concolic_builtin_hook(ctx, slot, "CSS Typed OM 1 §4.3.3 convert a CSSUnitValue", example);
            DCHECK(!JS_IsUninitialized(value),
                   "solver/concolic.h's builtin seam refused an operand this body had already established is "
                   "unknown external input — the two tests read the same value one line apart");
        }
        JS_FreeValue(ctx, slot);
    }
    r = css_unit_value_new(ctx, value, want);
    JS_FreeCString(ctx, want);
    return r;
}

/* ---- the per-agent declaration ------------------------------------------------------------------------------ */

int css_numeric_value_member_id(CssNumericMember m)
{
    DCHECK((unsigned)m < (unsigned)CSS_NUMERIC_MEMBER_N,
           "a §4.3.1 member id was asked for by a name outside this component's enumeration");
    DCHECK(g_id[m] >= 0,
           "a §4.3.1 member id was read before css_numeric_value_init declared it. The realm install that "
           "reads these runs from core/css/css_unit_value.c, whose platform row is ordered AFTER this "
           "component's, so an undeclared id is that ordering having been changed");
    return g_id[m];
}

void css_numeric_value_init(JSContext *ctx)
{
    /* `CSSUnitValue to(USVString unit)` — one required position, so Web IDL §3.7.7 Operations' `length` is 1. */
    static const IdlArgType TO_ARGS[1] = { IDL_USVSTRING };

    DCHECK(g_id[CSS_NUMERIC_MEMBER_TYPE] < 0 && g_id[CSS_NUMERIC_MEMBER_TO] < 0,
           "css_numeric_value_init ran twice in one agent — a second declaration would leave the first pool "
           "entry reachable by nothing and the install reading whichever ran last");
    /* `CSSNumericType type()` takes no arguments. */
    g_id[CSS_NUMERIC_MEMBER_TYPE] = idl_method_id(ctx, NULL, 0, js_css_numeric_value_type, 0);
    g_id[CSS_NUMERIC_MEMBER_TO]   = idl_method_id(ctx, TO_ARGS, 1, js_css_numeric_value_to, 0);
    DCHECK(g_id[CSS_NUMERIC_MEMBER_TYPE] >= 0 && g_id[CSS_NUMERIC_MEMBER_TO] >= 0,
           "one of this component's two declarations did not enter the argument pool");
    agent_state_id("css_numeric_value", &g_id[CSS_NUMERIC_MEMBER_TYPE],
                   "CSS Typed OM 1 §4.3.1's type() declaration");
    agent_state_id("css_numeric_value", &g_id[CSS_NUMERIC_MEMBER_TO],
                   "CSS Typed OM 1 §4.3.1's to() declaration");
}

/* THE INVERSE. Nothing here is a realm's — the interface prototype objects these members land on belong to
   core/css/css_unit_value.c — so what this component holds for the agent is the two pool entries and nothing
   else. */
void css_numeric_value_free(void)
{
    DCHECK(g_id[CSS_NUMERIC_MEMBER_TYPE] >= 0 && g_id[CSS_NUMERIC_MEMBER_TO] >= 0,
           "CSS Typed OM 1 §4.3.1's members were released in an agent that never declared them");
    g_id[CSS_NUMERIC_MEMBER_TYPE] = g_id[CSS_NUMERIC_MEMBER_TO] = -1;
}
