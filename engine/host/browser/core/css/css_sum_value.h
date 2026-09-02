/* CSS TYPED OM 1 §4.3.1 Common Numeric Operations, and the CSSNumericValue Superclass's SUM VALUE — "an
 * abstract representation of a CSSNumericValue as a sum of numbers with (possibly complex) units", and the
 * four algorithms stated over it: create a sum value, create a CSSUnitValue from a sum value item, create a
 * type from a unit map, and the product of two unit maps.
 *
 * WHY IT IS A COMPONENT AND NOT MORE OF core/css/css_numeric_value.c. That file owns §4.3.1's ELEVEN MEMBERS —
 * each one a body with a realm, a `this`, an argument list and a Web IDL brand check. The sum value is a
 * DIFFERENT KIND OF THING: a pure data structure with no JS surface at all, which takes a CSSNumericValue and
 * answers a list of (value, unit map) tuples or failure. §4.3.1 itself sets them apart — the members are
 * stated as "When called, it must perform the following steps" and the sum value is stated as "an abstract
 * representation of a CSSNumericValue as a sum of numbers with (possibly complex) units" — and so does the
 * failure mode: a member gets a page's argument wrong and throws, while this gets a UNIT MAP wrong and two
 * values that are the same value stop being one item. It is also the piece with TWO consumers already — the
 * step of `to()` and the step of `toSum()` that each read "Let sum be the result of creating a sum value from
 * this", their step 2 in both cases — which is the point at which a private helper inside one member's file
 * becomes a component.
 *
 * A UNIT MAP IS CANONICALLY ORDERED HERE, AND THAT IS A DECISION WITH AN ARGUMENT RATHER THAN A CONVENIENCE.
 * §4.3.1 calls it "a map of units (strings) to powers (integers)" and Infra maps are ORDERED by insertion, so
 * a faithful port could have kept insertion order. It must not, for the reason CLAUDE.md states about an
 * ordinal over a set: the ONE thing the algorithms ask of a unit map is whether "values already contains an
 * item with the same unit map as subvalue", and an insertion-ordered comparison answers NO for two maps that
 * differ only in the order the page's own expression happened to build them — `calc(1px * 1em)` against
 * `calc(1em * 1px)` — so `new CSSMathSum(a, b)` would keep two items where a browser keeps one, and `to()`
 * would then throw a TypeError on a value it must convert. Sorting by unit in code point order makes "the same
 * unit map" a fact about CONTENT, which is what the question means.
 *
 * THE ORDER IS UNOBSERVABLE EVERYWHERE ELSE, AND THAT IS WHAT LICENSES THE SORT. The only algorithm that
 * ITERATES a unit map is create a type from a unit map, which multiplies the resulting types together, and
 * NEITHER of that multiply's two failure arms is reachable from here. §4.3.2 Numeric Value Typing's own arm is
 * "If both type1 and type2 have non-null percent hints with different values", while §4.3.2's
 * create-a-type-from-a-string-unit ends with "In all cases, the associated percent hint is null" — so every
 * type entering that multiplication has a null hint. Its second arm is core/css/css_math.h's: a FAILURE
 * operand absorbs, since §4.3.2's failure became a representable value of a type when §4.3.1's toSum forced it
 * to. That arm is unreachable here too, and for a reason that is one line rather than an argument:
 * create-a-type-from-a-string-unit answers a type or REFUSES — it never answers a failure type — so the
 * accumulator and every operand of this fold are types. What is left in both cases is exponent addition, which
 * is order-independent. A sort would NOT have been safe under an operation that could fail on one ordering and
 * not another, so a future arm that CAN fail here is a change to this file's canonical order and not only to
 * its arithmetic.
 *
 * NO ENTRY EVER HAS A POWER OF ZERO, AND IT IS ASSERTED RATHER THAN NORMALISED AT EVERY WRITE. The leaf arm
 * writes power 1, invert negates powers, and the ONLY place a zero can be produced is the product of two unit
 * maps — which is also the one place §4.3.1 states the removal, "with all entries with a zero value removed".
 * So the removal stands exactly where the specification puts it and every other site DCHECKs the invariant
 * instead of re-imposing it; a zero surviving anywhere would make «["px" → 0]» and «[]» two different maps
 * that are the same map, which is the merge failing silently one level down.
 *
 * THE VALUE IS A JSValue BECAUSE §4.3.3's `value` internal slot IS ONE. core/css/css_unit_value.h states why —
 * unknown external input crosses Web IDL's boundary as itself so opacity survives the coercion — and every
 * arithmetic step here is therefore either a real number or a derivation through solver/concolic.h naming its
 * operands in order, never a collapse to the example.
 *
 * NOT ONE ARM OF create-a-sum-value BRANCHES ON A VALUE, WHICH IS WHY NOTHING HERE FORKS. The tests the six
 * arms make are about LIST LENGTHS and UNIT MAPS — "If the length of values is more than one, return failure",
 * "If not all of the unit maps among the items of args are identical, return failure" — and those are facts
 * about strings and integers this component computed, never about a number a page supplied. The one place a
 * reader expects a comparison over values is the min/max arm, and it is not one: its preceding step has
 * already refused every argument whose unit map differs, so "Return the item of args whose sole item has the
 * smallest value" selects among items that differ ONLY in their value, and the item it selects is exactly
 * «(the smallest of those values, the common unit map)». That is a VALUE computed from all of them — a
 * derivation naming every operand in order — and not a branch, so an unknown among them stays unknown instead
 * of deciding a question the page never asked. Contrast §4.3.1's invert, whose "If this's value internal slot
 * is set to 0 or -0, throw a RangeError" IS a branch over a value and is a DFAIL in core/css/
 * css_numeric_value.c naming the step machine it needs: THIS section's CSSMathInvert arm has no zero test at
 * all ("Invert (find the reciprocal of) the value of the item in values"), so 1/0 is IEEE-754's infinity here
 * and adding a test would be inventing a refusal no standard states.
 *
 * THE WALK IS ITERATIVE ON A HEAP STACK, for the reason core/css/css_math_value.c's §6.5 walk and core/css/
 * css_numeric_value.c's equal-numeric-values walk are: §4.3.1 states create a sum value recursively over a
 * tree whose DEPTH THE PAGE PICKS (`for (…) v = new CSSMathNegate(v)`), so a C function calling itself here
 * would be a self-contained recursion of unbounded depth — what engine/check_recursion.mjs fails by name and
 * what CLAUDE.md's flat-C-stack rule forbids outright. There is no depth cap either; the stack grows to the
 * allocation floor, which is the honest limit. */
#ifndef ENGINE_HOST_BROWSER_CORE_CSS_CSS_SUM_VALUE_H
#define ENGINE_HOST_BROWSER_CORE_CSS_CSS_SUM_VALUE_H

#include <stdbool.h>

#include "quickjs.h"

/* ONE ENTRY OF A UNIT MAP — §4.3.1's "a map of units (strings) to powers (integers)". `unit` is an OWNED,
   NUL-terminated copy of a §4.3.3 `unit` internal slot's own bytes, or of the canonical unit of the
   compatible-unit set that slot's unit belongs to. `power` is never 0 (see the header). */
typedef struct {
    char *unit;
    int   power;
} CssSumUnit;

/* A UNIT MAP: its entries sorted by `unit` in code point order, with no duplicate key. */
typedef struct {
    CssSumUnit *e;
    int         n, cap;
} CssUnitMap;

/* ONE ITEM OF A SUM VALUE — §4.3.1's "a tuple of a value, which is a number, and a unit map". `value` is
   OWNED and is either a Number or unknown external input, which is what §4.3.3's `value` internal slot holds
   and what every step here therefore has to carry through. */
typedef struct {
    JSValue    value;
    CssUnitMap units;
} CssSumItem;

/* §4.3.1's SUM VALUE — "A sum value is a list." */
typedef struct {
    CssSumItem *v;
    int         n, cap;
} CssSumValue;

/* §4.3.1's "To create a sum value from a CSSNumericValue this", ALL SEVEN ARMS — the CSSUnitValue leaf and
   §4.3.4 Complex Numeric Values: CSSMathValue objects' CSSMathSum, CSSMathNegate, CSSMathProduct,
   CSSMathInvert, CSSMathMin and CSSMathMax.
   FALSE is that algorithm's own "return failure", and `*out` is then left EMPTY rather than untouched, so a
   caller that releases unconditionally is correct either way. `v` must be a CSSNumericValue.
   THE THREE WAYS IT ANSWERS FAILURE ARE THE SPEC'S THREE, and none of them is a capability this engine is
   missing: a CSSMathSum whose merged unit maps have no combined type, a CSSMathInvert over more than one item,
   and a CSSMathMin/Max whose arguments are not all one item with one identical unit map. Anything else that
   cannot be answered CRASHES rather than joining them, because "not all CSSNumericValues can be expressed as a
   sum value" is a statement about the VALUES and must never become a statement about this port. */
bool css_sum_value_create(JSContext *ctx, JSValueConst v, CssSumValue *out);

/* THE INVERSE, over a sum value in any state create-a-sum-value can leave one in, including empty. */
void css_sum_value_release(JSContext *ctx, CssSumValue *s);

/* §4.3.1's "When asked to create a CSSUnitValue from a sum value item item", all three arms — the refusal for
   a unit map with more than one entry, the "number" answer for one with none, and the refusal for a sole entry
   whose power is anything other than 1.
   JS_UNDEFINED is that algorithm's "return failure"; OWNED otherwise. `item` is BORROWED — the value is dup'd
   into the CSSUnitValue — so the sum value it belongs to is still the caller's to release. */
JSValue css_sum_value_unit_value(JSContext *ctx, const CssSumItem *item);

#endif
