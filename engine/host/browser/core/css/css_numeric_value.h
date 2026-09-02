/* CSS TYPED OM 1 §4.3.1 Common Numeric Operations, and the CSSNumericValue Superclass — the operations every
 * numeric CSS value can perform — over §4.3.2 Numeric Value Typing's TYPE, which every one of them is stated
 * in terms of.
 *
 * WHY IT IS A COMPONENT BESIDE core/css/css_unit_value.c AND NOT MORE OF IT. That file is §4.3.3 Value + Unit:
 * CSSUnitValue objects: a record with two internal slots, its finalizer, its COW layout and its §6.4
 * serialization — one assertable contract, and every line of it is about the object's STATE. This file is
 * about OPERATIONS OVER numeric values, which is a different contract with a different failure mode: §4.3.1
 * declares eleven members, and every one of them is stated over a TYPE and a SUM VALUE rather than over a
 * record. The two did not stay the same size, which is what the split was made in advance of: §4.3.1's
 * arithmetic is six members of joint typing, and its sum value is a whole component of its own.
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
 * WHAT IS HONESTLY ABSENT. Of §4.3.1's eleven members this file installs ten: `type()`, `to()`, `equals()`,
 * `toSum()` and the six arithmetic operations. The static `parse()` is NOT installed and is not a stub — it
 * needs CSS Syntax 3's parse-a-component-value and §5.6 <number>, <percentage>, and <dimension> values'
 * reification, neither of which this file can reach. A page that touches it gets the TypeError a browser
 * without it gives, which is the forcing function.
 *
 * AND `to()` NO LONGER HAS A RECEIVER IT CANNOT ANSWER FOR. Its step 2 is stated over create-a-sum-value,
 * which is core/css/css_sum_value.h's component; both this member and `toSum()` reach it and NEITHER
 * has an arm of its own for a §4.3.4 receiver. The collapsed CSSUnitValue path that used to stand here — one
 * conversion ratio composing create-a-sum-value's canonicalisation with §4.3.3's convert, under an `if` on the
 * receiver's class — is DELETED rather than kept beside the general one: it was correct, and a second
 * implementation of one algorithm selected by a predicate is the shape CLAUDE.md names, whose gaps the
 * surviving arm hides. What is left of it is `css_numeric_convert_ratio` below, which the general path's LAST
 * step ("then converting it to unit") is stated in terms of and which core/css/css_sum_value.c reaches for the
 * canonical unit of a compatible set.
 *
 * WHAT UNBLOCKED THE SIX. All seven of §4.3.1's `CSSNumberish`-taking members waited on TWO things, and they
 * were different things. The first was a Web IDL union core/idl_args.h had no row for: a member that declared
 * it `IDL_ANY` and sorted the arms in its body would run the page's `valueOf` from a plain C activation and
 * would get `equals(null)` and `equals(undefined)` wrong in opposite directions. That row is
 * IDL_DOUBLE_UNLESS_IFACE. The second was §4.3.4 itself — each of the six arithmetic operations RETURNS a
 * CSSMathValue on the arm where its operands do not collapse to one unit, so a member answering only the
 * collapsing arm would be WRONG for input a browser answers rather than narrower. `equals` owed §4.3.4 nothing
 * because it answers a `boolean`, which is why it landed first; the six landed with core/css/css_math_value.c. */
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

/* §4.3.3's "To convert a CSSUnitValue this to a unit unit" AS ITS FACTOR — the ratio that section's last step
 * multiplies by, which is the whole of what §4.3.1's `to()` does to a value once create-a-sum-value has handed
 * it a CSSUnitValue, and the whole of what `toSum()`'s "If value unit is a compatible unit with unit, then:
 * Convert value to unit" asks.
 *
 * IT ALSO ANSWERS WHETHER TWO UNITS ARE COMPATIBLE AT ALL, WHICH IS THE SAME QUESTION AND NOT A SECOND ONE:
 * css-values-4 §5.4.1 Compatible Units defines compatibility AS being "related by a static multiplicative
 * factor", so a factor exists exactly when the two units are compatible and `toSum`'s membership test is this
 * entry's own true/false rather than a predicate beside it.
 *
 * IT USED TO COMPOSE TWO CONVERSIONS — create-a-sum-value's canonicalisation and this one — because `to()`
 * collapsed both into one ratio for the only receiver it could answer for. It does not any more: the
 * canonicalisation belongs to core/css/css_sum_value.c, which asks `css_numeric_canonical` below for the
 * canonical unit BY NAME because that name is what goes into a unit map.
 *
 * FALSE IS §4.3.3's "If old unit and unit are not compatible units, return failure", which §4.3.1's `to()`
 * step 3 turns into a TypeError and which `toSum()` reads as this value not being in the unit it is testing;
 * nothing is written.
 *
 * IT IS THE ONE THING IN THIS COMPONENT THAT IS A PURE FUNCTION OF TWO UNIT IDENTIFIERS, which is why it is an
 * entry rather than a private helper: the member bodies need a realm, a `this` and a page, and this does not,
 * so it is the part a fixture can hold to a known answer (96 px to the inch, 1000 Hz to the kilohertz, the
 * refusal of `em` to `px`) with nothing under it. */
bool css_numeric_convert_ratio(const char *from, const char *to, double *ratio);

/* css-values-4 §5.4.1 Compatible Units' SET, asked of one unit: which unit is the canonical unit of the set
 * `unit` belongs to, and how many of THAT unit one of `unit` is worth. FALSE — writing nothing — for a unit in
 * no set at all ("number", "percent", `em`, `cqw`, `fr`), which is a POSITIVE statement that the unit is
 * compatible with itself alone and not an absence a caller has to default past. The unit is a SPAN and the
 * five tables it is asked of match unit identifiers ASCII case-insensitively; `canon` is a static literal.
 *
 * IT IS PUBLIC BECAUSE §4.3.1's SUM VALUE ASKS THIS EXACT QUESTION AND `css_numeric_convert_ratio` CANNOT
 * ANSWER IT. That entry composes two conversions into one ratio between two units the CALLER names, and
 * create-a-sum-value's CSSUnitValue arm names only one of them — "if unit is a member of a set of compatible
 * units, and is not the set's canonical unit, multiply value by the conversion ratio between unit and the
 * canonical unit, and change unit to the canonical unit" — so it needs the set's canonical unit as a NAME,
 * which is what goes into the unit map. A second table in core/css/css_sum_value.c would be the sixth copy of
 * the five families' rows, and it would be the copy that disagrees about `dpcm` the day one of them is
 * edited. */
bool css_numeric_canonical(const char *unit, size_t unit_len, double *per, const char **canon);

/* Web IDL §3.2.15 Interface types' "If V implements I" FOR I = CSSNumericValue — the predicate the
   `(double or CSSNumericValue)` union's arm is, and the one §3.7.7 Operations brand every member of this
   superclass checks its receiver with. ONE entry, because those are the same question asked at two ends of a
   call and a second spelling of it is the copy that goes on admitting one subclass after another is minted.
   IT TAKES A REALM AND IGNORES IT, which is core/idl_args.h's `idl_arg_iface` signature and is deliberate
   there: an interface reached through a PROTOTYPE CHAIN is a per-realm fact. This one is not — it is a class
   brand, and a class id belongs to the runtime — so the parameter is accepted and unread rather than the
   declaration being handed a predicate that cannot answer for the interfaces that do need a realm.
   IT IS THE DISJUNCTION OF THE SUBCLASS BRANDS AND MUST GROW WITH THEM. §4.3.3's CSSUnitValue and §4.3.4's six
   CSSMathValue subclasses are what "implements CSSNumericValue" means in this engine today; §4.3.4's
   CSSMathClamp is the one the standard declares and core/css/css_math_value.h states the absence of. A brand
   left out here is a platform object the union's arm silently rejects, which a page sees as a TypeError on a
   value its own constructor just handed it. */
bool css_numeric_value_is(JSContext *ctx, JSValueConst v);

/* CSS Typed OM 1 §4.3 Numeric Values:'s "To rectify a numberish value num" — "If num is a CSSNumericValue,
   return num. If num is a double, return a new CSSUnitValue with its value internal slot set to num and its
   unit internal slot set to unit" (the optional unit defaulting to "number").
   PUBLIC BECAUSE §4.3.4's CONSTRUCTORS PERFORM IT TOO — every one of them opens with "Replace each item of
   args with the result of rectifying a numberish value for the item", which is the same sentence §4.3.1's
   seven `CSSNumberish`-taking members open with. One speller, because the union's two arms are read off the
   value and a second reader is what admits a third thing.
   OWNED either way: the CSSNumericValue arm DUPS rather than borrowing, so one release frees whichever arm
   ran and no caller has to know which. */
JSValue css_numeric_value_rectify(JSContext *ctx, JSValueConst num);

/* THE TYPE OF ANY CSSNumericValue — §4.3.3's "The type of a CSSUnitValue is the result of creating a type from
   its unit internal slot" and §4.3.4's "The type of a CSSMathValue depends on its class", as the ONE question
   every algorithm stated over "the type of this" asks. `v` must be a CSSNumericValue.
   IT CANNOT FAIL AND SO ANSWERS THE TYPE ITSELF. A CSSUnitValue's unit is one §4.3.2's create-a-type admits —
   §4.3.3's constructor throws for any other and §4.3.5's factories name units the tables carry — and a
   CSSMathValue's type was computed and stored by its own constructor, whose failure arm threw before the
   object existed. A unit that has no type is therefore a mint that went past its own step 1, which is a DFAIL
   at that site and not a value for this entry to answer. */
CssMathType css_numeric_value_type_of(JSContext *ctx, JSValueConst v);

/* §4.3.1's MEMBERS THIS COMPONENT DECLARES, named so the one realm install can spell each install site with a
   literal IDL name beside the id it was declared under. */
typedef enum {
    CSS_NUMERIC_MEMBER_TYPE = 0,   /* `CSSNumericType type()` */
    CSS_NUMERIC_MEMBER_TO,         /* `CSSUnitValue to(USVString unit)` */
    CSS_NUMERIC_MEMBER_TOSUM,      /* `CSSMathSum toSum(USVString... units)` */
    CSS_NUMERIC_MEMBER_EQUALS,     /* `boolean equals(CSSNumberish... value)` */
    /* §4.3.1's SIX ARITHMETIC OPERATIONS, in the order its IDL declares them. They are six ids and not one,
       because Web IDL §3.7.7 Operations gives each its own function object with its own `name`, even where
       four of them share a body magic'd by the operator. */
    CSS_NUMERIC_MEMBER_ADD,        /* `CSSNumericValue add(CSSNumberish... values)` */
    CSS_NUMERIC_MEMBER_SUB,        /* `CSSNumericValue sub(CSSNumberish... values)` */
    CSS_NUMERIC_MEMBER_MUL,        /* `CSSNumericValue mul(CSSNumberish... values)` */
    CSS_NUMERIC_MEMBER_DIV,        /* `CSSNumericValue div(CSSNumberish... values)` */
    CSS_NUMERIC_MEMBER_MIN,        /* `CSSNumericValue min(CSSNumberish... values)` */
    CSS_NUMERIC_MEMBER_MAX,        /* `CSSNumericValue max(CSSNumberish... values)` */
    CSS_NUMERIC_MEMBER_N
} CssNumericMember;

/* The pool id a member was declared under, for core/css/css_unit_value.c's realm install. */
int css_numeric_value_member_id(CssNumericMember m);

void css_numeric_value_init(JSContext *ctx);
void css_numeric_value_free(void);

#endif
