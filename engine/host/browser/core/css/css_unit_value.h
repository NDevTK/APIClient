/* CSS TYPED OM — the value objects, and the first three of its class hierarchy: CSS Typed OM Level 1 §2
 * CSSStyleValue objects, §4.3.1 Common Numeric Operations, and the CSSNumericValue Superclass, §4.3.3 Value +
 * Unit: CSSUnitValue objects, and §6.4 CSSUnitValue Serialization.
 *
 * WHY THE TYPE COMES BEFORE THE SIXTY-THREE FACTORIES THAT MOTIVATE IT. §4.3.5 Numeric Factory Functions is by
 * far the largest single family the Web IDL gap audit reports absent from one interface, and it is not
 * sixty-three algorithms: the section states ONE behaviour for all of them — "All of the above methods must,
 * when called with a double value, return a new CSSUnitValue whose value internal slot is set to value and
 * whose unit internal slot is set to the name of the method as defined here" — and then says outright that
 * the naming is a shorthand "to avoid defining the unit individually for all ~60 functions". So the family is
 * a TABLE over one body, and the only thing it needs that this engine did not have is the object it returns.
 * That is why this component exists before core/css/css_namespace.c grows the table, and it is also why the
 * factories cannot be written first: there would be nothing for them to return but a shape.
 *
 * THE CHAIN IS BUILT WHOLE BECAUSE §3.7.3 Interface prototype object MAKES IT ONE OBJECT GRAPH. `CSSUnitValue`
 * inherits `CSSNumericValue` inherits `CSSStyleValue`, and core/idl_args.c asserts that link per realm off
 * the §3.7.3 Interface prototype object class string each prototype carries. So a CSSUnitValue.prototype
 * cannot be installed over %Object.prototype% "for now": the assert names both interfaces and fires. Both
 * bases are therefore real
 * interface prototype objects with real interface objects on the global — which is also what a page's
 * `x instanceof CSSNumericValue` reads, and browser/platform_names.h already lists all three as names the
 * platform owns, so solver/absent.c answers a CONCRETE `undefined` for them rather than unknown input and
 * every `if (window.CSSUnitValue)` in a bundle is decided against the engine today.
 *
 * WHAT IS HONESTLY ABSENT AND WHY THAT IS NOT A STUB. §2's `parse`/`parseAll` and every one of §4.3.1's eleven
 * members (add, sub, mul, div, min, max, equals, to, toSum, type, and the static parse) are NOT installed.
 * They are not noops and not opaque getters — they are not there, so a page that reaches one gets the
 * TypeError a browser missing them would give, which is the forcing function this project runs on. The audit
 * reports them as the gaps they are on two rows that did not exist before this component; that is existing
 * work becoming visible rather than new work appearing.
 *
 * THE `value` SLOT IS A JSValue AND NOT A `double`, WHICH IS THE ONE DESIGN DECISION IN THIS FILE.
 * §4.3.3 declares `attribute double value` — WRITABLE — so the slot is mutable shared state that must ride the
 * running flow's COW delta, and it is also a position unknown external input reaches: `CSS.px(el.dataset.n * 2)`
 * hands the factory a concolic, and core/idl_args.h's numeric conversion states the rule for that in its own
 * words — "unknown external input crosses a boundary AS ITSELF … so that opacity survives the coercion". A
 * `double` field would have to collapse it to a number, which deletes the fork and every arm behind it. So the
 * slot holds either the Number the declaration produced or the unknown itself, `value` reads back what was
 * written, and §6.4's serialization derives an unknown STRING from an unknown value (never a de-tainting
 * placeholder) through solver/concolic.h's builtin seam.
 *
 * §6.4's NUMBER IS CSSOM's, NOT A SECOND PRINTER. "Set s to the result of serializing a <number> from value,
 * per CSSOM § 6.7.2 Serializing CSS Values" is the same operation core/css/css_length.h already performs for a
 * length and a percentage, so this component reaches it rather than formatting a double of its own — a second
 * shortest-round-trip printer is the copy that disagrees about `0.1 + 0.2` while producing a string that LOOKS
 * like CSS either way. */
#ifndef ENGINE_HOST_BROWSER_CORE_CSS_CSS_UNIT_VALUE_H
#define ENGINE_HOST_BROWSER_CORE_CSS_CSS_UNIT_VALUE_H

#include <stdbool.h>
#include <stddef.h>

#include "quickjs.h"

/* CSS Typed OM 1 §4.3.2 Numeric Value Typing's "create a type from a string unit", asked only for whether it
 * FAILS — which is the whole of what §4.3.3's constructor needs ("If creating a type from unit returns
 * failure, throw a TypeError and abort this algorithm"). The map itself is CSSNumericValue's `type()` and
 * that member is not built, so answering the exponents here would be a value with no reader.
 *
 * SEVEN OF ITS NINE BRANCHES ARE css_math_unit_base's; the two that are not are "number" and "percent", and
 * they stand here because they are NOT dimension units — see core/css/css_math.h for why answering them there
 * would make `calc(5number)` a `<number>`.
 *
 * THE UNIT IS A SPAN AND IS COMPARED ASCII CASE-INSENSITIVELY, like every other unit question in this engine.
 * §4.3.2's own branches are stated over the unit PRODUCTIONS, and CSS matches unit identifiers
 * case-insensitively, which is why `new CSSUnitValue(1, "PX")` is a length. The two literal branches are the
 * exception the spec writes literally — "unit is "number"" — and are compared as the spec spells them.
 *
 * NAMED RESIDUAL — CSS CONDITIONAL 5 §7's SIX CONTAINER-RELATIVE UNITS ARE NOT IN THIS ENGINE'S `<length>`
 * VOCABULARY, SO THIS ENTRY REFUSES THEM.
 * WHAT IS NOT COVERED: `cqw`, `cqh`, `cqi`, `cqb`, `cqmin` and `cqmax` are `<length>` units that CSS
 * Conditional 5 §7 Container Relative Lengths: the cqw, cqh, cqi, cqb, cqmin, cqmax units defines, and
 * core/css/css_length.h's unit set — css-values-4 §6.2's seven absolute units, §6.1.1's twelve font-relative
 * ones and §6.1.2's viewport-percentage family in its four spellings — does not contain them. So
 * `new CSSUnitValue(5, "cqw")` throws the TypeError §4.3.3 step 1 gives for a unit that has no type, where a
 * browser returns a value.
 * WHAT THE NEXT DIFF BUILDS: the six as members of that set, so `css_length_is_length_unit` admits them and
 * `css_math_unit_base` therefore answers `<length>` for them — which is one table and not seven, since every
 * other question in the engine reads the same set. Absolutizing one is a SECOND thing and is not required for
 * this: a unit value carries a unit and resolves nothing, so the absolutization arm may name the query
 * container it needs and abort there exactly as css-values-4 §6.1.2.1's small, large and dynamic viewport
 * families already do at the same switch.
 * HOW ITS ABSENCE WOULD SHOW: `CSS.cqw(5)` SUCCEEDS while `new CSSUnitValue(5, "cqw")` throws, which is the
 * two spellings of one value disagreeing — §4.3.5's factories are defined with no create-a-type step at all
 * (see css_unit_value.c), so they neither consult this entry nor are fixed by fixing it. */
bool css_unit_value_type_is_valid(const char *unit, size_t unit_len);

/* §4.3.3's spec-internal "create a CSSUnitValue from a pair (num, unit)" — "return a new CSSUnitValue object
 * with its value internal slot set to num, and its unit internal slot set to unit". `value` is CONSUMED and is
 * either a Number or unknown external input; `unit` is COPIED and must be one of §4.3.5's names or a unit this
 * engine's tables know.
 *
 * IT RUNS NO TYPE CHECK, AND THAT IS §4.3.5's OWN SHAPE RATHER THAN A SHORTCUT. The constructor's step 1 is
 * the only place §4.3.3 states one; the factory functions are defined as "return a new CSSUnitValue whose …
 * unit internal slot is set to the name of the method", with no create-a-type in the sentence at all. A check
 * here would therefore refuse `CSS.cqw(5)`, which the standard requires to work. */
JSValue css_unit_value_new(JSContext *ctx, JSValue value, const char *unit);

/* Web IDL §3.7.5's BRAND: is this object a CSSUnitValue of this agent? */
bool css_unit_value_is(JSValueConst v);

/* CSS Typed OM 1 §6 CSSStyleValue Serialization over one value — §6.4's arm, which is the only one this engine
 * has a subclass for. OWNED: a JS string, or the unknown a §6.4 run over an unknown `value` slot derives.
 * `v` must be a CSSUnitValue; the caller has already asked css_unit_value_is. */
JSValue css_unit_value_serialize(JSContext *ctx, JSValueConst v);

void css_unit_value_init(JSContext *ctx);
void css_unit_value_free(void);

#endif
