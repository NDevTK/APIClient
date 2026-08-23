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
 * IT HANDS OUT §5.4.1'S SYNTAX DEFINITION — "an object consisting of a list of syntax components" — because
 * §3.3 has to decide whether a VALUE parses "according to the rule's syntax descriptor", and §4.1 spells out
 * that the non-universal arm of that is "according to syntax definition". That decision is the one thing the
 * components are for, and it is made in core/css/css_syntax_match.h, over a CSS VALUE. This file stops where
 * §5.4 stops: it reads a STRING and produces the definition, and nothing in it knows what a `<length>` is.
 * The UNIVERSAL definition carries no components at all — §5.4.1 calls it "a special syntax definition which
 * accepts any valid token stream", so it is a different answer and not an empty list of the same one, and the
 * two are told apart by a flag rather than by a count.
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

/* §5.2's two MULTIPLIERS, which §5.4.1 makes the optional half of a syntax component. */
typedef enum {
    CSS_SYNTAX_MULT_NONE = 0,
    CSS_SYNTAX_MULT_SPACE,   /* §5.2's U+002B PLUS SIGN (+): "Indicates a space-separated list." */
    CSS_SYNTAX_MULT_COMMA    /* §5.2's U+0023 NUMBER SIGN (#): "Indicates a comma-separated list." */
} CssSyntaxMultiplier;

/* §5.4.1's SYNTAX COMPONENT — "an object consisting of a syntax component name, and an optional multiplier".
   The name carries its ANGLE BRACKETS when it is a data type name, because §5.4.4 appends both and then asks
   whether what it built is one of §5.1's fifteen; an ident component's name is the UNESCAPED ident sequence
   (`I\ dent` is the six-code-point name `I dent`), which is what §5.1's codepoint-wise comparison is against.
   A PRE-MULTIPLIED name keeps CSS_SYNTAX_MULT_NONE — §5.4.3 returns the component BEFORE looking for a
   multiplier, and its own multiplication is part of what the name means (§5.1: "<transform-list> is a
   pre-multiplied data type name equivalent to <transform-function>+"), so a matcher reads it out of the name
   and this field never restates it. */
typedef struct {
    char               *name;         /* OWNED */
    CssSyntaxMultiplier multiplier;
} CssSyntaxComponent;

/* §5.4.1's SYNTAX DEFINITION. `universal` is step 3's lone `*` — §5.4.1's "special syntax definition which
   accepts any valid token stream" — and it carries NO components, which is asserted rather than assumed
   because "accepts everything" and "accepts nothing" would otherwise be the same list. */
typedef struct {
    CssSyntaxComponent *v;
    size_t              n;
    bool                universal;
} CssSyntaxDefinition;

/* §5.4.2's CONSUME A SYNTAX DEFINITION over `s`/`len`. FALSE is the algorithm's own FAILURE — the string is not
   a syntax string, which §3.1 reads as "ignore this descriptor" and §4.1 as a SyntaxError — and it allocates
   NOTHING, so a refused definition needs no free (the out-parameter is zeroed either way). On TRUE the caller
   owns `*out` and frees it with the entry below. */
bool css_property_syntax_definition(const char *s, size_t len, CssSyntaxDefinition *out);

void css_syntax_definition_free(CssSyntaxDefinition *d);

#endif
