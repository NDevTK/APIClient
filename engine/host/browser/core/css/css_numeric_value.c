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

static int g_id[CSS_NUMERIC_MEMBER_N] = { -1, -1, -1 };

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

/* See css_numeric_value.h, which carries this predicate's named residual — it is stated there because the
   union arm the declaration names reads it from outside this file and the residual is about the ANSWER, not
   about either caller. */
bool css_numeric_value_is(JSContext *ctx, JSValueConst v)
{
    (void)ctx;
    return css_unit_value_is(v);
}

/* Web IDL §3.7.5 Operations' brand check: a member of CSSNumericValue.prototype reached on something that is
 * not a CSSNumericValue is a TypeError, and a page tells that apart from `undefined`. It is §3.2.15's own
 * question, so it is the entry above and never a second test — the receiver of `equals` and its arguments are
 * branded against one predicate, which is what stops the two from admitting different sets of objects. */
static bool nv_brand(JSContext *ctx, JSValueConst v)
{
    if (css_numeric_value_is(ctx, v)) return true;
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
       from here — see css_numeric_value.h's residual on `css_numeric_value_is` for the arm that makes them
       reachable. What is left is §4.3.3's
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

/* ---- §4.3's rectify a numberish value, and §4.3.1's `equals` ----------------------------------------------- */

/* THE RELATION §4.3.1's equality PERFORMS, as the name solver/concolic.h's `concolic_new_rel` composes into a
   predicate's identity. It is the SPEC ALGORITHM and not the C operator, because that identity is what the
   flow's path constraint is keyed by: two different comparisons of one pair of values must not compose to one
   key, or a flow's record of either decides the other. */
#define NV_EQUAL_REL "CSS Typed OM 1 §4.3.1 equal numeric values"

/* CSS Typed OM 1 §4.3 Numeric Values:'s "To rectify a numberish value num, optionally to a given unit unit
 * (defaulting to "number"), perform the following steps" — over an argument whose union §3.2.25 has ALREADY
 * resolved, so the two branches are read off the value rather than sorted here: "If num is a CSSNumericValue,
 * return num." and "If num is a double, return a new CSSUnitValue with its value internal slot set to num and
 * its unit internal slot set to unit."
 *
 * THE OPTIONAL UNIT IS NOT A PARAMETER YET, and that is a statement about the callers rather than a narrowing.
 * §4.3.1's six arithmetic members and its `equals` all rectify at the default; the members that pass one are
 * §4.4 CSSTransformValue objects' components, none of which this engine builds. The day one of those is built
 * this grows the parameter — writing `"number"` at every call site instead is the same fact seven sites are
 * each asked to remember.
 *
 * OWNED EITHER WAY. The CSSNumericValue arm DUPS rather than borrowing, so one release frees whichever arm
 * ran and no caller has to know which. */
static JSValue nv_rectify(JSContext *ctx, JSValueConst num)
{
    if (css_numeric_value_is(ctx, num)) return JS_DupValue(ctx, num);
    /* §3.2.7's `double` produces a Number, and Web IDL's boundary passes unknown external input across as
       itself so opacity survives the coercion — which is exactly what §4.3.3's `value` internal slot is a
       JSValue to hold. A third thing means the position was declared as something other than the union. */
    DCHECK(JS_IsNumber(num) || concolic_is(num),
           "§4.3's rectify-a-numberish-value was handed a value that is neither a CSSNumericValue nor a "
           "double. core/idl_args.h's IDL_DOUBLE_UNLESS_IFACE resolves §3.2.25's two arms before any body "
           "runs — the interface arm crosses the platform object as itself and the other is §3.2.7's "
           "restricted double — so anything else is a member that declared this position as some other type");
    return css_unit_value_new(ctx, JS_DupValue(ctx, num), "number");
}

/* §4.3.1's "equal ... value internal slots" ASKED OF THE VALUE SLOTS ALONE, over the two things §4.3.3's slot
 * can hold. 1 equal, 0 not equal, and -1 for the residue this component cannot answer by arithmetic: a
 * comparison one of whose operands is unknown external input, which the caller mints a predicate for.
 *
 * THE SAME VALUE IS EQUAL WHATEVER IT IS, so identity is asked first: without it `x.equals(x)` would carry a
 * predicate with one feasible answer, and a derivation this engine cannot spell would be reported undecided
 * against ITSELF. Two unknowns that are the same DERIVATION are likewise one value — solver/concolic.h
 * composes identity at every derivation precisely so that a question about one is a question about the other —
 * and that second test is what keeps `CSS.px(x).equals(CSS.px(1), CSS.px(1))` one question rather than two.
 *
 * A CONCOLIC AND A NUMBER ARE NEVER "NOT EQUAL" HERE. The unknown may be any number at all, including that
 * one, so 0 would be a control-flow decision over unknown input — the collapse §@S forbids — where -1 is the
 * honest "this needs a predicate". */
static int nv_slot_equal(JSContext *ctx, JSValueConst a, JSValueConst b)
{
    bool ca = concolic_is(a), cb = concolic_is(b);
    double x = 0.0, y = 0.0;

    if (ca != cb) return -1;
    if (ca) {
        const char *ia, *ib;

        /* ONE OBJECT IS ONE VALUE. The pointer test is asked of concolics ONLY and never of the numeric case:
           a JSValue's payload union is a pointer for an object and the double itself for a Number, so on a
           32-bit target two different doubles sharing their low word would compare identical. */
        if (JS_VALUE_GET_PTR(a) == JS_VALUE_GET_PTR(b)) return 1;
        ia = concolic_ident_c(a);
        ib = concolic_ident_c(b);
        return (ia != NULL && ib != NULL && strcmp(ia, ib) == 0) ? 1 : -1;
    }
    DCHECK(JS_IsNumber(a) && JS_IsNumber(b),
           "§4.3.1's equality was asked about a `value` internal slot that is neither a Number nor unknown "
           "external input — §4.3.3's mint asserts those are the only two things that reach a slot, so a "
           "third here is a record written past that mint");
    JS_ToFloat64(ctx, &x, a);
    JS_ToFloat64(ctx, &y, b);
    /* §3.2.7's restricted `double` refused a NaN at every boundary that writes this slot, so `==` is total
       here and the reflexive case above has already answered the one pair it would get wrong. */
    return x == y ? 1 : 0;
}

/* THE PREDICATE nv_slot_equal's -1 STANDS FOR, with §4.3.1's equality run on the operands' own examples as its
   example. The example is what marks the arm a real session takes PRIMARY at the page's own branch; it is
   attached only when BOTH operands carry one, because `idl_number_of` answering false means there is no
   concrete the code computed and a number chosen here would be invented. */
static JSValue nv_slot_predicate(JSContext *ctx, JSValueConst a, JSValueConst b)
{
    JSValue pred = concolic_new_rel(ctx, NV_EQUAL_REL, a, b);
    double x = 0.0, y = 0.0;

    if (idl_number_of(ctx, IDL_DOUBLE, a, &x) && idl_number_of(ctx, IDL_DOUBLE, b, &y))
        concolic_set_example(ctx, pred, JS_NewBool(ctx, x == y));
    return pred;
}

/* "The equals(...values) method, when called on a CSSNumericValue this, must perform the following steps:
   Replace each item of values with the result of rectifying a numberish value for the item. For each item in
   values, if the item is not an equal numeric value to this, return false. Return true."
   Its equality is §4.3.1's own, and that section says how exacting it is meant to be: "This notion of equality
   is purposely fairly exacting; all the values must be the exact same type and value, in the same order."

   §4.3.1's equal-numeric-values REACHES ITS SECOND STEP FOR EVERY PAIR THIS ENGINE CAN BUILD. "If value1 and
   value2 are not members of the same interface, return false" is step 1, and both operands here are
   CSSUnitValues — `this` because §3.7.5's brand admits nothing else, and each item because §4.3's rectify
   returns either the argument (a CSSNumericValue, which today is a CSSUnitValue) or a fresh CSSUnitValue — so
   step 2 is what runs: "If value1 and value2 are both CSSUnitValues, return true if they have equal unit and
   value internal slots, or false otherwise." See css_numeric_value.h's residual on `css_numeric_value_is` for
   the diff that makes step 1 able to answer false.

   THE UNITS ARE COMPARED BY CODE POINTS AND NOT CASE-INSENSITIVELY, which is a real divergence from §4.3.2 one
   member over: create-a-type-from-a-string-unit matches unit IDENTIFIERS the way CSS does, so `new
   CSSUnitValue(1, "PX")` is a length, while §4.3.1 says "equal unit ... internal slots" and §4.3.3's
   constructor sets that slot to the argument it was given. So `CSS.px(1).equals(new CSSUnitValue(1, "PX"))` is
   FALSE by the standard's own two sentences, and it is written that way here rather than smoothed.

   THE LOOP IS SCANNED WHOLE RATHER THAN SHORT-CIRCUITED, AND THAT IS SOUND BECAUSE STEP 1 ALREADY RAN. §4.3.1
   rectifies EVERY item before it compares any of them, and rectification of an already-converted argument
   observes nothing and runs no page code — so the order the comparisons are made in is unobservable, and a
   definite `false` found at the LAST item is the algorithm's answer whatever the earlier items did. That is
   what lets `CSS.px(x).equals(CSS.px(1), CSS.em(1))` answer a plain false instead of forking over the first
   item and answering false on both arms.

   TWO UNDECIDED COMPARISONS ARE ONE VALUE AND IT IS THEIR CONJUNCTION, minted by solver/concolic.h's
   `concolic_new_conj` — which is what lets this loop hold a residue of ANY size in one place. Every
   comparison here shares `this`'s slot, so two of them ask ONE question exactly when the two items' slots are
   one value; that used to be tested here, against the FIRST undecided item's slot, and that test is GONE
   rather than narrowed. The mint's identity is the SET of conjuncts, so two comparisons that ask one question
   compose one member and collapse back to the single predicate — the identical answer, decided by comparing
   the two predicates' own identities instead of by this component re-deriving when two slots are one value.

   THE RESULT OF AN UNDECIDED COMPARISON IS THE PREDICATE ITSELF, not a decision taken here. `equals` IS a
   comparison, so its unknown answer belongs at the page's own `if` — where §7.1.2 ToBoolean's branch seam
   forks it and files ONE constraint entry that `if (a.equals(b))` shares with every other read of that
   predicate — rather than inside a plain C activation, which has no frame for a sibling to be snapshotted at
   and would fork over a value the page may never branch on. It is the shape core/dom/page_visibility.c's
   `hidden` already answers `document.visibilityState === "hidden"` with, and for the same sentence of the
   spec: a member whose IDL type is `boolean` and whose definition is a comparison. */
static JSValue js_css_numeric_value_equals(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                                           int magic)
{
    JSValue this_slot, held = JS_UNDEFINED;
    const char *this_unit;
    int i;

    (void)magic;
    if (!nv_brand(ctx, this_val)) return JS_EXCEPTION;
    this_unit = css_unit_value_unit(this_val);
    this_slot = css_unit_value_value(ctx, this_val);
    for (i = 0; i < argc; i++) {
        JSValue item = nv_rectify(ctx, argv[i]);   /* step 1, for this item */
        JSValue slot;
        int r;

        DCHECK(css_numeric_value_is(ctx, item),
               "§4.3's rectify-a-numberish-value answered with something that is not a CSSNumericValue — both "
               "its branches return one (the argument itself, or a fresh CSSUnitValue), so this is that entry "
               "having grown a third");
        /* STEP 2's TWO CONJUNCTS. The unit decides on its own — two different units are "false otherwise"
           whatever the values are — and only a matching unit leaves the value slots to answer. */
        {
            bool same_unit = strcmp(this_unit, css_unit_value_unit(item)) == 0;

            slot = css_unit_value_value(ctx, item);
            JS_FreeValue(ctx, item);
            r = same_unit ? nv_slot_equal(ctx, this_slot, slot) : 0;
        }
        if (r == 0) {                              /* step 2's "return false", for this item */
            JS_FreeValue(ctx, slot);
            JS_FreeValue(ctx, held);
            JS_FreeValue(ctx, this_slot);
            return JS_FALSE;
        }
        if (r > 0) { JS_FreeValue(ctx, slot); continue; }
        /* THIS ITEM'S OWN QUESTION, AND THEN THE CONJUNCTION OF IT WITH EVERYTHING STILL UNDECIDED. The mint
           holds a SET, so an item asking a question an earlier item already asked adds no member and the
           conjunction collapses back to the predicate that was already held — `CSS.px(x).equals(CSS.px(1),
           CSS.px(1))` costs one entry, and `CSS.px(1).equals(CSS.px(x), CSS.px(y))` costs one entry naming
           both. The fold's own shape does not reach the key: the set is canonically ordered and flattened, so
           the arguments' order and this loop's left-association are unobservable in it — which is what makes
           a fold here sound rather than a second spelling of an N-ary primitive. */
        {
            JSValue q = nv_slot_predicate(ctx, this_slot, slot);

            JS_FreeValue(ctx, slot);
            if (JS_IsUndefined(held)) { held = q; continue; }
            {
                JSValue both = concolic_new_conj(ctx, held, q);
                JS_FreeValue(ctx, held);
                JS_FreeValue(ctx, q);
                held = both;
            }
        }
    }
    JS_FreeValue(ctx, this_slot);
    /* "Return true" — reached when every item's comparison answered equal, which for an empty argument list is
       vacuously every one of them. `held` is the undecided residue where there was one. */
    return JS_IsUndefined(held) ? JS_TRUE : held;
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

/* THE NAME EVERY §3.2.15 TypeError FROM THIS UNION IS ABOUT. It is the IDL's own identifier, so a page that
   passes the wrong thing is told which interface it failed, and it must outlive the declaration. */
static const char NV_IFACE[] = "CSSNumericValue";

void css_numeric_value_init(JSContext *ctx)
{
    /* `CSSUnitValue to(USVString unit)` — one required position, so Web IDL §3.7.7 Operations' `length` is 1. */
    static const IdlArgType TO_ARGS[1] = { IDL_USVSTRING };
    /* `boolean equals(CSSNumberish... value)` — ONE declared position carrying the tail's type, which is what
       `T...` means to core/idl_args.h: the last declared type applies to every argument from there on. §3.7.7
       Operations' `length` is 0, because a variadic tail is not a required position. */
    static const IdlArgType EQUALS_ARGS[1] = { IDL_DOUBLE_UNLESS_IFACE };
    unsigned m;

    for (m = 0; m < CSS_NUMERIC_MEMBER_N; m++)
        DCHECKF(g_id[m] < 0,
                "css_numeric_value_init found member %u already declared. Either it ran twice in one agent — "
                "a second declaration leaves the first pool entry reachable by nothing and the install reading "
                "whichever ran last — or the id table's initialiser is shorter than CssNumericMember, which "
                "zero-fills the tail and 0 is a VALID pool id", m);
    /* `CSSNumericType type()` takes no arguments. */
    g_id[CSS_NUMERIC_MEMBER_TYPE] = idl_method_id(ctx, NULL, 0, js_css_numeric_value_type, 0);
    g_id[CSS_NUMERIC_MEMBER_TO]   = idl_method_id(ctx, TO_ARGS, 1, js_css_numeric_value_to, 0);
    g_id[CSS_NUMERIC_MEMBER_EQUALS] =
        idl_method_id(ctx, EQUALS_ARGS, 1, js_css_numeric_value_equals, 0);
    idl_variadic();
    /* §3.2.15's `I` FOR THE UNION'S ARM, AS A PREDICATE AND NOT A CLASS — core/idl_args.h's own split.
       CSSNumericValue is an interface no single class id names: CSSUnitValue is one class today and §4.3.4's
       six CSSMathValue subclasses will each be another, and "implements" is the question all of them answer
       yes to. `idl_iface_brand` could name only the one that exists, which is a narrowing this file would then
       have to widen from two places. */
    idl_arg_iface(0, css_numeric_value_is, NV_IFACE);
    for (m = 0; m < CSS_NUMERIC_MEMBER_N; m++)
        DCHECKF(g_id[m] >= 0, "this component's declaration of member %u did not enter the argument pool", m);
    agent_state_id("css_numeric_value", &g_id[CSS_NUMERIC_MEMBER_TYPE],
                   "CSS Typed OM 1 §4.3.1's type() declaration");
    agent_state_id("css_numeric_value", &g_id[CSS_NUMERIC_MEMBER_TO],
                   "CSS Typed OM 1 §4.3.1's to() declaration");
    agent_state_id("css_numeric_value", &g_id[CSS_NUMERIC_MEMBER_EQUALS],
                   "CSS Typed OM 1 §4.3.1's equals() declaration");
}

/* THE INVERSE. Nothing here is a realm's — the interface prototype objects these members land on belong to
   core/css/css_unit_value.c — so what this component holds for the agent is the two pool entries and nothing
   else. */
void css_numeric_value_free(void)
{
    unsigned m;

    for (m = 0; m < CSS_NUMERIC_MEMBER_N; m++) {
        DCHECKF(g_id[m] >= 0,
                "CSS Typed OM 1 §4.3.1's member %u was released in an agent that never declared it", m);
        g_id[m] = -1;
    }
}
