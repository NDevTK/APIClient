/* CSS TYPED OM — CSS Typed OM 1 §4.2 "CSSKeywordValue objects" and §6.2 "CSSKeywordValue Serialization".
 * The interface prototype object these members land on is built by core/css/css_unit_value.c, because §4.2
 * declares `CSSKeywordValue : CSSStyleValue` and Web IDL §3.7.3 "Interface prototype object" makes that one
 * object graph with the chain that file already creates — the same split core/css/css_math_value.h states for
 * §4.3.4, and for the same reason: two components ordering two intrinsics independently are two files that
 * have to agree about an order.
 *
 * WHY THIS INTERFACE AND NOT §2's `parse`. The Web IDL gap audit reports §2's CSSStyleValue as missing BOTH of
 * the members it declares, and reports the same two on every subclass below it, so `parse` reads as the whole
 * of the CSS Typed OM gap. It is not buildable yet and this interface is what it is waiting on. §2's
 * "parse a CSSStyleValue" ends by REIFYING its parsed result, §5 "CSSStyleValue Reification" is the algorithm
 * that does it, and §5.5 "Identifier Values" — the arm §5.1 "Property-specific rules" names for `display`,
 * `position`, `float`, `visibility` and most of the table — says "To reify an identifier ident, return a new
 * CSSKeywordValue with its value internal slot set to the serialization of ident". There was no such object to
 * return. So a `parse` landed before this would have had to answer the commonest property in CSS with either a
 * throw or a wrong class, which is §NO STUBS' hazard rather than a narrower engine.
 *
 * AND IT IS INVISIBLE TO THE AUDIT THAT MOTIVATES IT, WHICH IS THE POINT OF SAYING SO HERE. `CSSKeywordValue`
 * occurs ZERO times in engine/idlgen.mjs's whole report: a member-list diff walks the interfaces it can FIND,
 * so an interface with no component and no Web IDL §3.7.3 tag contributes no row and no absent member — while
 * browser/platform_names.h, browser/idl_exposure.h and browser/idl_inheritance.h have all three named it for
 * as long as they have existed. A page's `new CSSKeywordValue("block")` was the loudest failure available, a
 * ReferenceError on the line that names it, and no count anywhere held it.
 *
 * THE `value` SLOT IS A JSValue, FOR core/css/css_unit_value.h's ARGUMENT ABOUT ITS OWN. §4.2 declares
 * `attribute USVString value` — WRITABLE — so it is mutable shared state that must ride the running flow's COW
 * delta, and it is a position unknown external input reaches: `new CSSKeywordValue(location.hash.slice(1))`
 * hands the constructor a concolic, and core/idl_args.h's string boundary passes such input across AS ITSELF.
 * A `char *` field would have to collapse it to bytes, which deletes the fork and every arm behind it.
 *
 * §6.2 NEEDS NO DERIVATION, AND THAT IS A DIFFERENCE FROM §6.4 RATHER THAN AN OMISSION. §6.4 "CSSUnitValue
 * Serialization" COMPUTES a string out of a number, so over an unknown it must derive one through
 * solver/concolic.h's builtin seam or it would de-taint. §6.2 is one sentence — "Return this's value internal
 * slot" — and performs no operation at all, so the serialization of a keyword whose slot is unknown IS that
 * unknown, carried out unchanged. Reaching for the builtin seam here would name an operation the standard does
 * not state and would put a second identity on a value that already has one.
 *
 * WHAT IS DELIBERATELY NOT BUILT: §4.2's "rectify a keywordish value". It is not absent for want of work —
 * it has NO CALLER in this engine and cannot acquire one from this diff. The `CSSKeywordish` typedef it exists
 * to serve is named by exactly two things in the harvested IDL this project consumes, §4.4's CSSPerspective
 * and §4.6's CSSColor, and neither interface is built; a member of this component with no reader is the shape
 * core/idl_args.h's own gap rules call untested code wearing a completed subproblem. It arrives WITH the first
 * interface that declares a `CSSKeywordish` position, which is what gives it a caller in the same diff.
 * HOW ITS ABSENCE WOULD SHOW: it cannot show today, because no member of this engine accepts a CSSKeywordish
 * for it to be reached through. It would show the day such a member is built and passes a raw DOMString
 * straight into a slot §4.2 says must hold a CSSKeywordValue. */
#ifndef ENGINE_HOST_BROWSER_CORE_CSS_CSS_KEYWORD_VALUE_H
#define ENGINE_HOST_BROWSER_CORE_CSS_CSS_KEYWORD_VALUE_H

#include <stdbool.h>

#include "quickjs.h"

/* §4.2's MINT — "return a new CSSKeywordValue with its value internal slot set to value". `value` is CONSUMED
 * and is either a JS string or unknown external input; the caller has already made §4.2's empty-string check,
 * which is the constructor's and the setter's step and not the mint's.
 *
 * PUBLIC AHEAD OF ITS SECOND CALLER DELIBERATELY, AND THE CALLER IS NAMED: §5.5 "Identifier Values"' reify an
 * identifier is exactly this call over the serialization of an ident, and it is the reason this component
 * exists. Until §5 is built the constructor and the setter are the only mints. */
JSValue css_keyword_value_new(JSContext *ctx, JSValue value);

/* Web IDL §3.8 "Platform objects implementing interfaces"' "value implements an interface interface": is this
   object a CSSKeywordValue of this agent? core/css/css_unit_value.c's §6 dispatch asks it to choose §6.2's
   arm, and Web IDL §3.7.6 "Attributes"' TypeError is what the two accessors owe a receiver that fails it. */
bool css_keyword_value_is(JSValueConst v);

/* CSS Typed OM 1 §6.2 "CSSKeywordValue Serialization" — "To serialize a CSSKeywordValue this: Return this's
 * value internal slot." OWNED: the slot's own value, dup'd — a JS string, or the unknown the slot holds.
 * `v` must be a CSSKeywordValue; the caller has already asked css_keyword_value_is. */
JSValue css_keyword_value_serialize(JSContext *ctx, JSValueConst v);

/* THE PER-REALM INSTALL, CALLED BY core/css/css_unit_value.c WITH CSSStyleValue's INTERFACE PROTOTYPE OBJECT.
   It is not a realm intrinsic of its own for core/css/css_math_value.h's reason: Web IDL §3.7.3 "Interface
   prototype object" makes
   CSSStyleValue → CSSKeywordValue one graph, so the component that creates the base creates the order too.
   `style_value_proto` is BORROWED — the caller still owns it and still frees it. */
void css_keyword_value_install_realm(JSContext *ctx, JSValueConst style_value_proto);

void css_keyword_value_init(JSContext *ctx);
void css_keyword_value_free(void);

#endif
