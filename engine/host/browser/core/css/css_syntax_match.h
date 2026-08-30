/* CSS Properties and Values API 1 §5 "Syntax Strings", the half that reads a CSS VALUE — §5.1's fifteen
 * supported names and the types each accepts, §5.2's two multipliers, and §5.3's ordered combinator.
 *
 * WHY IT IS NOT IN core/css/css_property_syntax.c. That component's own header states the property this one
 * would destroy: §5.4 is "an algorithm over code points and not over CSS values, which is what makes it
 * separable at all" — it consumes ident sequences and data type names and never asks what a `<length>` is. The
 * moment a `<length>` grammar is in the same translation unit, that sentence stops being true and the syntax
 * STRING parser starts carrying every value grammar CSS has. So the two are split exactly where §5 splits: the
 * definition is produced there and consumed here.
 *
 * WHO ASKS. §3.3 "The initial-value Descriptor" — "if specified, the value of the initial-value descriptor
 * must successfully parse according to the rule's syntax descriptor, or else the descriptor is invalid and
 * ignored" — and §4.1's register a custom property, whose step spells out what "according to" means:
 * "parse initialValue according to <declaration-value>? if syntax definition is the universal syntax
 * definition, and according to syntax definition otherwise." This component owns the SECOND arm only. The
 * first is not a syntax question at all and is decided where the value comes from: a value that reached a
 * CSSPropertyRule was produced by parsing the rule's body as declarations, so it already IS a
 * `<declaration-value>`.
 *
 * A MATH FUNCTION IS A VALUE OF WHICHEVER NUMERIC TYPE ITS OPERANDS GIVE IT, so every numeric name below has a
 * FUNCTION arm and none of them has a table of function names. CSS Values and Units 4 §10.9 "Type Checking"
 * decides which type any given one is, over the whole component value rather than over its lead token, and
 * core/css/css_math.h answers it. §5.1's own split between `<length>` and `<length-percentage>` is passed down
 * as the CALCULATION CONTEXT §10.9.1 says a math function inherits, because `calc(1px + 50%)` matches the
 * second and not the first and no OR of the two could say so.
 *
 * A SYNTAX COMPONENT MATCHES EXACTLY ONE CSS COMPONENT VALUE, and that is the invariant the whole file rests
 * on rather than a simplification of it. Every one of §5.1's fifteen types is a single CSS Syntax token or a
 * single functional notation — `10px`, `"s"`, `#fff`, `rgb(1 2 3)`, `url(a.png)`, `rotate(45deg)`,
 * `calc(1px + 2px)` — so §5.2's `+` is a whitespace-separated run of component values and its `#` a
 * comma-separated one, and the unmultiplied case is one component value with nothing after it. Nothing in §5.1
 * spans two of them, so there is no backtracking here and no ambiguity to resolve. The day §5.1 grows a name
 * that DOES span two, the walk here is what has to change — and the crash that says so is the one at the end of
 * the per-type dispatch, which fires for any supported name this file does not decide.
 *
 * A TYPE THIS ENGINE HAS NO GRAMMAR FOR CRASHES AT THE VALUE THAT NEEDS IT, never quietly refuses. Answering
 * "does not match" for `<transform-function>` would turn an unbuilt component into a rule the page's own
 * `initialValue` reports as absent — a plausible datum indistinguishable from a measurement — so a name whose
 * grammar is a whole component in its own right aborts by name, and the abort says which component to build.
 * Refusing is reserved for values the grammars below actually decided.
 *
 * AND THE ABORT NAMES A COMPONENT THAT DOES NOT EXIST YET, WHICH IS A CLAIM ABOUT THIS TREE AND SO GOES STALE.
 * `<image>` used to abort here saying this engine had no `<image>` grammar, and core/css/css_image.c — the
 * component that very crash asked for — was built without the crash being deleted, so the abort stood telling
 * the next reader to build what was already there. It is now ROUTED to that component. Before writing the code
 * one of these aborts asks for, grep for the entry it names; when you build one, delete the crash AND the
 * prose agreeing with it in the same diff. */
#ifndef ENGINE_HOST_BROWSER_CORE_CSS_CSS_SYNTAX_MATCH_H
#define ENGINE_HOST_BROWSER_CORE_CSS_CSS_SYNTAX_MATCH_H

#include <stdbool.h>
#include <stddef.h>

#include "core/css/css_property_syntax.h"

/* Does `value`/`len` parse according to `d` — §5.3's "when a syntax definition with multiple syntax components
   is used to parse a CSS value, the syntax components are matched in the order specified", so the first
   component that matches decides and the rest are never asked.
   `d` MUST NOT be §5.4.1's universal syntax definition: §4.1 parses against that one as `<declaration-value>?`,
   which is a different production and a different caller's answer, and handing it here would silently ask the
   component grammars a question the universal definition does not pose. It is asserted, not tolerated. */
bool css_property_syntax_matches(const CssSyntaxDefinition *d, const char *value, size_t len);

#endif
