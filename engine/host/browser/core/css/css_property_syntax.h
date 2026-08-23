/* CSS Properties and Values API 1 §5 "Syntax Strings" — the string a custom property registration declares its
 * accepted value types with, and §5.4 "Parsing The Syntax String", which is the only thing that decides whether
 * one is a syntax string at all.
 *
 * WHY IT IS A COMPONENT AND NOT A LINE IN THE `@property` RULE. Two specifications state a validity condition
 * BY REFERENCE to this one algorithm and neither restates it: §3.1 "The syntax Descriptor" — "If the provided
 * string is not a valid syntax string (if it returns failure when consume a syntax definition is called on it),
 * the descriptor is invalid and must be ignored" — and §4.1 "The registerProperty() Function" — "Attempt to
 * consume a syntax definition from syntax. If it returns failure, throw a SyntaxError." Two callers and one
 * algorithm, so a second copy could disagree about which of them accepts `I\ dent|none`.
 *
 * IT IS AN ALGORITHM OVER CODE POINTS AND NOT OVER CSS VALUES, which is what makes it separable at all. §5.4.2
 * through §5.4.4 read the syntax STRING: they consume ident sequences, data type names out of §5.1's closed
 * list, the `+`/`#` multipliers of §5.2 and the `|` combinator of §5.3. Nothing in them parses a CSS value, so
 * this answers `syntax: "<length>"` with no `<length>` grammar under it.
 *
 * IT HANDS OUT NO PARSED FORM, for the reason core/css/css_at_rule_prelude.h gives for a page selector list:
 * the syntax definition's COMPONENTS have no consumer in this build, and a parsed form with one reader is a
 * second representation that drifts from the one that is used. What crosses is the two facts §3.1 and §3.3 ask
 * for — whether the string is one at all, and whether it is §5.4.1's UNIVERSAL SYNTAX DEFINITION, which is the
 * definition that "accepts any valid token stream" and therefore the one under which every `<declaration-value>`
 * parses. The day a caller has to decide whether a VALUE matches a non-universal definition, that caller needs
 * the components, and they are built here where §5.4.3 already produces them.
 *
 * A DATA TYPE NAME IS MATCHED CODEPOINT-WISE. §5.4.4 appends the code points it consumed and then asks whether
 * "name is a supported syntax component name"; §5.1 spells all fifteen in lower case and neither section folds
 * case, where §5.1's own note folds nothing either ("<custom-ident>s are compared codepoint-wise with each
 * other; this is different than the normal behavior of UA-defined CSS which limits itself to ASCII and is ASCII
 * case-insensitive"). So `<LENGTH>` is not a supported name. */
#ifndef ENGINE_HOST_BROWSER_CORE_CSS_CSS_PROPERTY_SYNTAX_H
#define ENGINE_HOST_BROWSER_CORE_CSS_CSS_PROPERTY_SYNTAX_H

#include <stdbool.h>
#include <stddef.h>

/* §5.4.2's CONSUME A SYNTAX DEFINITION over `s`/`len`. FALSE is the algorithm's own FAILURE — the string is not
   a syntax string, which §3.1 reads as "ignore this descriptor" and §4.1 as a SyntaxError. On true, `*puniversal`
   is whether the result is §5.4.1's universal syntax definition (step 3's lone `*`) rather than a list of
   components; a caller that does not care still passes a bool, because the two outcomes are not the same answer
   and a NULL out-parameter would let one be mistaken for the other. */
bool css_property_syntax_definition(const char *s, size_t len, bool *puniversal);

#endif
