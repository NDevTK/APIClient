/* css-variables-1 §3 "Using Cascading Variables: the var() notation" — THE SYNTAX HALF, and only that.
 *
 * "The value of a custom property can be substituted into the value of another property with the var()
 * function." This file finds those functions in a declaration value and splices the substitution in. It knows
 * nothing about elements, the cascade, inheritance or which custom properties exist: every NAME it meets is
 * handed back to a resolver the caller supplies, so the question "what is `--x` here" is answered by the
 * component that owns the tree and this one is exercisable with one fixture and no DOM.
 *
 * WHY THE SPLIT IS HERE AND NOT SOMEWHERE ELSE. Substitution is two problems that fail differently. The
 * SYNTAX problem is where a `var(` starts and ends — which is a question about strings, nesting and ident
 * boundaries, and gets `content: "var(--x)"` and `calc(var(--w) * 2)` wrong in ways a fixture can catch. The
 * POLICY problem is what a name resolves to, which needs the element, §7.2's inheritance and the cycle rule,
 * and is wrong in ways only a tree can show. One file answering both would be a component no fixture can
 * exercise, and core/css/css_computed_value.c is where the policy half lives because that is where the
 * cascaded value and the parent chain already are.
 *
 * THE FAILURE ARM IS THE WHOLE POINT AND IT IS NOT A FALLBACK TO THE PARENT. css-variables-1 §2.2
 * "Guaranteed-Invalid Values": "The initial value of a custom property is a guaranteed-invalid value", and
 * "If it ever appears in a property value, then at computed value time that property becomes invalid at
 * computed-value time". css-values-5 defines that term in its "Appendix A: Arbitrary Substitution Functions"
 * — an APPENDIX, so there is no section number to cite and the title is the citation — and its Note says what
 * happens next: "the property falls back (essentially) to unset behavior, rather than falling back to an
 * earlier value in the cascade the way declarations invalid at parse time do". So a NULL from
 * `css_var_substitute` is CSS Cascade 5 §7.3.3's `unset` and never the parent's computed value and never the
 * next declaration down. Getting that arm wrong is the difference between a styled page and a black one.
 *
 * NAMED RESIDUAL — THE OTHER ARBITRARY SUBSTITUTION FUNCTIONS ARE NOT HERE. WHAT IS NOT COVERED: css-values-5
 * calls `var()` one `arbitrary substitution function` among several and states one machinery for all of them
 * (`replace an arbitrary substitution function`, `substitute arbitrary substitution functions`); this file
 * implements `var()` alone, by name. WHAT THE NEXT DIFF BUILDS: that machinery as the shared step, with
 * `var()` as its first caller rather than its only one, which is also what `attr()` and `if()` need. HOW ITS
 * ABSENCE WOULD SHOW, as an observation: a declaration whose value contains a substitution function this file
 * does not name is passed through unchanged and then fails its own property's grammar, so it computes as the
 * property's initial or inherited value with nothing anywhere saying a substitution was owed.
 * RETIREMENT: this record goes when the scan is over the function SET rather than over one name.
 */
#ifndef ENGINE_HOST_BROWSER_CORE_CSS_CSS_VAR_H
#define ENGINE_HOST_BROWSER_CORE_CSS_CSS_VAR_H
#include <stdbool.h>
#include <stddef.h>

/* What a resolver can answer about one `<custom-property-name>`. Three states and not two, because
   css-values-5 makes a CYCLE behave differently from an unset property: an unset one is §2.2's
   guaranteed-invalid value and "the fallback, if any, is used instead", while a var() in one of its
   `cyclic substitution contexts` is invalid at computed-value time outright. Folding them would make
   `--a: var(--b); --b: var(--a)` answer with `--a`'s fallback, which is a value no browser produces. */
typedef enum {
    CSS_VAR_RESOLVED = 0,        /* `*out` holds the property's substituted value, OWNED and freed by this file */
    CSS_VAR_GUARANTEED_INVALID,  /* §2.2's value — the var()'s fallback is used if it states one */
    CSS_VAR_CYCLE                /* the whole declaration is invalid at computed-value time; no fallback */
} CssVarResolution;

/* Answer `name` (WITHOUT its leading `--`, `name_n` bytes, NOT NUL-terminated) for the caller's element.
   A resolver that answers CSS_VAR_RESOLVED must leave `*out` owned and non-NULL; the other two leave it
   untouched. The resolver is what performs the RECURSION — a custom property's own value may reference
   another — so that the cycle stack lives with the policy rather than being a parameter of the syntax. */
typedef CssVarResolution (*CssVarResolve)(void *ud, const char *name, size_t name_n, char **out);

/* Does `value` contain a `var(` function token at all? Asked before the work, because the overwhelming
   majority of declarations contain none and this answer is what keeps substitution off that path. It is the
   SAME scan `css_var_substitute` performs, so the two cannot come to disagree about what a function token is:
   a `var(` inside a string is not one, and neither is the `(` of an ident that merely ends in `var`. */
bool css_var_references(const char *value);

/* §3's substitution over the whole of `value`, however deeply the functions nest. Returns OWNED text, or NULL
   when the value is INVALID AT COMPUTED-VALUE TIME — which is a resolver's CSS_VAR_CYCLE, or a `var()` whose
   property is guaranteed-invalid and which states no fallback, or a `var()` whose first argument is not a
   `<custom-property-name>` at all. A value carrying no `var(` is returned as a copy, so every caller frees
   exactly one thing on every path. */
char *css_var_substitute(const char *value, CssVarResolve resolve, void *ud);

#endif
