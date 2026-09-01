/* CSS TYPED OM 1 §4.3.1 Common Numeric Operations, and the CSSNumericValue Superclass — the operations every
 * numeric CSS value can perform — over §4.3.2 Numeric Value Typing's TYPE, which every one of them is stated
 * in terms of.
 *
 * WHY IT IS A COMPONENT BESIDE core/css/css_unit_value.c AND NOT MORE OF IT. That file is §4.3.3 Value + Unit:
 * CSSUnitValue objects: a record with two internal slots, its finalizer, its COW layout and its §6.4
 * serialization — one assertable contract, and every line of it is about the object's STATE. This file is
 * about OPERATIONS OVER numeric values, which is a different contract with a different failure mode: §4.3.1
 * declares eleven members and the ones this engine can answer today are the ones whose algorithm terminates
 * inside the one subclass that exists. The two will not stay the same size — §4.3.1's arithmetic is six
 * members of joint typing and sum values — so they are split before the split has to be done under a diff.
 *
 * THE INTERFACE PROTOTYPE OBJECT IS STILL BUILT IN ONE PLACE, AND THAT IS DELIBERATE. Web IDL §3.7.3 Interface
 * prototype object makes CSSStyleValue → CSSNumericValue → CSSUnitValue ONE object graph, which is the reason
 * core/css/css_unit_value.c builds all three prototypes in one realm install; this component therefore hands
 * that install its member DECLARATIONS (`css_numeric_value_member_id`) rather than reaching for the prototype
 * itself. One place creates the chain, one place installs onto it, and engine/idl_installed.mjs reads the
 * §3.7.3 Interface prototype object tag off the same local the members land on.
 *
 * §4.3.2's TYPE IS css-values-4 §10.9 Type Checking's TYPE — the same map, the same three operations, and this
 * file does not own it. §10.9 links to §4.3.2 by name rather than restating it, core/css/css_math.h holds the
 * `CssMathType` those two sections share, and its add/multiply/invert are that section's algorithms. So the
 * only thing §4.3.2 states that the math component does not is the NINE-BRANCH create-a-type-from-a-string-
 * unit below, whose two literal branches ("number", "percent") are not dimension units and must not be
 * answered from a dimension table — see core/css/css_math.h for why that split is one table and two questions.
 *
 * WHAT IS HONESTLY ABSENT. Of §4.3.1's eleven members this file installs `type()` and `to()`. The six
 * arithmetic operations (add, sub, mul, div, min, max), `equals()` and `toSum()` are NOT installed and are not
 * stubs: each of them is defined to RETURN a §4.3.4 Complex Numeric Values: CSSMathValue object on the arm
 * where its operands do not collapse to one unit, and no component in this engine mints one, so a member that
 * answered for the collapsing arm alone would be wrong for input a browser answers rather than narrower. The
 * static `parse()` needs CSS Syntax 3's parse-a-component-value and §5.6 <number>, <percentage>, and
 * <dimension> values' reification, neither of which this file can reach. A page that touches one gets the
 * TypeError a browser without it gives, which is the forcing function. */
#ifndef ENGINE_HOST_BROWSER_CORE_CSS_CSS_NUMERIC_VALUE_H
#define ENGINE_HOST_BROWSER_CORE_CSS_CSS_NUMERIC_VALUE_H

#include <stdbool.h>
#include <stddef.h>

#include "quickjs.h"

#include "core/css/css_math.h"

/* §4.3.2's "To create a type from a string unit, follow the appropriate branch of the following" — all nine
 * branches, answering the TYPE and not merely whether the unit has one. §4.3.3's constructor asks only the
 * failure half ("If creating a type from unit returns failure, throw a TypeError"), and §4.3.1's `type()`
 * needs the map itself; one entry answers both, because a predicate that answers the narrower question is the
 * one that has to be widened the day the second caller arrives.
 *
 * FALSE IS §4.3.2's LAST BRANCH — "anything else → Return failure" — and nothing is written.
 *
 * SEVEN OF THE NINE BRANCHES ARE `css_math_unit_base`, over the unit tables the components that define them
 * own. The two that are not are "number" and "percent", and they stand HERE because they are not dimension
 * units at all: CSS Syntax 3 §4 tokenizes a percentage as its own token type, and `calc(5number)` is a
 * dimension token css-values-4 §10.9 Type Checking refuses.
 *
 * THE UNIT IS A SPAN, and the seven production branches compare it ASCII case-insensitively because CSS
 * matches unit identifiers that way — `new CSSUnitValue(1, "PX")` is a length. The two literal branches are
 * compared by code points instead, as §4.3.2 spells them: neither "number" nor "percent" is a unit identifier
 * that any dimension token can carry, so there is no case-insensitive matching rule that reaches them. */
bool css_numeric_type_from_unit(const char *unit, size_t unit_len, CssMathType *out);

/* §4.3.1's "create a sum value from this" (the CSSUnitValue arm) COMPOSED WITH §4.3.3's "convert a
 * CSSUnitValue this to a unit", as the one FACTOR their composition is — the whole of what `to()` does to a
 * value once §4.3.2 has admitted the unit, and the whole of what `toSum()`'s "If value unit is a compatible
 * unit with unit, then: Convert value to unit" will ask.
 *
 * FALSE IS §4.3.3's "If old unit and unit are not compatible units, return failure", which §4.3.1's `to()`
 * step 6 turns into a TypeError; nothing is written.
 *
 * IT IS THE ONE THING IN THIS COMPONENT THAT IS A PURE FUNCTION OF TWO UNIT IDENTIFIERS, which is why it is an
 * entry rather than a private helper: the member bodies need a realm, a `this` and a page, and this does not,
 * so it is the part a fixture can hold to a known answer (96 px to the inch, 1000 Hz to the kilohertz, the
 * refusal of `em` to `px`) with nothing under it. */
bool css_numeric_convert_ratio(const char *from, const char *to, double *ratio);

/* §4.3.1's MEMBERS THIS COMPONENT DECLARES, named so the one realm install can spell each install site with a
   literal IDL name beside the id it was declared under. */
typedef enum {
    CSS_NUMERIC_MEMBER_TYPE = 0,   /* `CSSNumericType type()` */
    CSS_NUMERIC_MEMBER_TO,         /* `CSSUnitValue to(USVString unit)` */
    CSS_NUMERIC_MEMBER_N
} CssNumericMember;

/* The pool id a member was declared under, for core/css/css_unit_value.c's realm install. */
int css_numeric_value_member_id(CssNumericMember m);

void css_numeric_value_init(JSContext *ctx);
void css_numeric_value_free(void);

#endif
