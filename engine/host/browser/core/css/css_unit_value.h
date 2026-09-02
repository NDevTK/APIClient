/* CSS TYPED OM — the value objects, and the first three of its class hierarchy: CSS Typed OM Level 1 §2
 * CSSStyleValue objects, §4.3.3 Value + Unit: CSSUnitValue objects, and §6.4 CSSUnitValue Serialization. §4.3.1
 * Common Numeric Operations, and the CSSNumericValue Superclass is core/css/css_numeric_value.h's; the
 * prototype its members land on is built here because §3.7.3 makes the three one object graph.
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
 * WHAT IS HONESTLY ABSENT AND WHY THAT IS NOT A STUB. §2's `parse`/`parseAll`, §4.3.1's `toSum` and its
 * static `parse` are NOT installed. They are not noops and not opaque getters — they are not there, so a page
 * that reaches one gets the TypeError a browser missing them would give, which is the forcing function this
 * project runs on. Nine of §4.3.1's eleven members ARE installed and live in core/css/css_numeric_value.c;
 * that file states what each of the two absentees is waiting on, and neither is waiting on this one.
 *
 * AND THIS FILE'S REALM INSTALL NOW BUILDS §4.3.4's CHAIN TOO, by calling core/css/css_math_value.c with the
 * CSSNumericValue.prototype it has just made. It is the same §3.7.3 Interface prototype object argument one
 * level down: a CSSMathSum.prototype inherits a CSSMathValue.prototype which inherits the object built here,
 * so one place creates the graph and each component installs its own members onto its own object.
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

/* §4.3.3's spec-internal "create a CSSUnitValue from a pair (num, unit)" — "return a new CSSUnitValue object
 * with its value internal slot set to num, and its unit internal slot set to unit". `value` is CONSUMED and is
 * either a Number or unknown external input; `unit` is COPIED and must be one of §4.3.5's names or a unit this
 * engine's tables know.
 *
 * IT RUNS NO TYPE CHECK, AND THAT IS §4.3.5's OWN SHAPE RATHER THAN A SHORTCUT. The constructor's step 1 is
 * the only place §4.3.3 states one; the factory functions are defined as "return a new CSSUnitValue whose …
 * unit internal slot is set to the name of the method", with no create-a-type in the sentence at all. A check
 * here would therefore refuse a unit that has no type, and §4.3.5 requires the factory for that unit to work.
 * WITH EVERY ONE OF §4.3.5's SIXTY-THREE NAMES NOW IN THE UNIT TABLES the two answers coincide, so nothing in
 * this engine currently WITNESSES the difference. It is the standard's shape and not an accommodation: the day
 * a specification names a `<length>` unit before core/css/css_length.h carries it, the factory for that unit
 * must still mint and only the constructor spelling may throw. */
JSValue css_unit_value_new(JSContext *ctx, JSValue value, const char *unit);

/* Web IDL §3.7.5's BRAND: is this object a CSSUnitValue of this agent? */
bool css_unit_value_is(JSValueConst v);

/* §4.3.3's TWO INTERNAL SLOTS, for the §4.3.1 algorithms that are stated over them — create a sum value from a
 * CSSUnitValue, convert a CSSUnitValue to a unit, the type of a CSSUnitValue, and equal numeric values — which
 * live in core/css/css_numeric_value.c because they belong to the SUPERCLASS. `v` must be a CSSUnitValue; the
 * caller has already asked css_unit_value_is.
 *
 * THE UNIT IS BORROWED AND THE VALUE IS OWNED, and the asymmetry is the slots' own. §4.3.3 declares `unit`
 * `readonly` and nothing rewrites it after the mint, so a pointer into the record stays valid for as long as
 * the object does. `value` is a WRITABLE attribute holding a counted reference that the setter may replace,
 * so it is handed out dup'd — a borrow of it would be a pointer into a slot a page's own assignment can free
 * while the algorithm that borrowed it is still running.
 *
 * BOTH GO THROUGH THIS COMPONENT'S COW CAPTURE, which is solver/cow.h's rule and is why they are entries and
 * not a struct in the header: a record a flow has REACHED is one it may write, so reaching it through anything
 * but this file would be a read the delta never saw. */
const char *css_unit_value_unit(JSValueConst v);
JSValue     css_unit_value_value(JSContext *ctx, JSValueConst v);

/* CSS Typed OM 1 §6 CSSStyleValue Serialization over one value — §6.4's arm, which is the only one this engine
 * has a subclass for. OWNED: a JS string, or the unknown a §6.4 run over an unknown `value` slot derives.
 * `v` must be a CSSUnitValue; the caller has already asked css_unit_value_is. */
JSValue css_unit_value_serialize(JSContext *ctx, JSValueConst v);

/* §6.4 step 3's THREE ARMS, as the one thing they differ in — the text appended after the digits. It is a list
   headed "If unit is:" whose arms are, for `"number"`, "Do nothing."; for `"percent"`, "Append "%" to s."; and
   for `anything else`, "Append unit to s."
   PUBLIC BECAUSE §6.5 REACHES §6.4 AT EVERY LEAF. CSS Typed OM 1 §6.5 CSSMathValue Serialization serializes
   each operand, and §6.3's dispatch sends a CSSUnitValue operand to §6.4 — so core/css/css_math_value.c writes
   the digits and this suffix for every leaf of a tree. A second copy of the three arms there would be the copy
   that disagrees about `"percent"` the day one of them is edited. BORROWED: the answer is either a literal or
   `unit` itself, and §4.3.3 declares that slot `readonly`. */
const char *css_unit_value_suffix(const char *unit);

void css_unit_value_init(JSContext *ctx);
void css_unit_value_free(void);

#endif
