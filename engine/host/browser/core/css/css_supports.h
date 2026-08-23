/* CSS Conditional Rules Module Level 3 §6 "Feature queries: the @supports rule" and §6.1 "Definition of
 * support" — the `<supports-condition>` grammar and the boolean each of its terms carries.
 *
 * IT IS A COMPONENT AND NOT A HELPER INSIDE css_rule.c, because THREE callers ask this one question and each
 * of them would otherwise answer it for itself: CSS Conditional §7.4's `CSSSupportsRule.matches` ("the
 * evaluation of the CSS feature query represented in conditionText"), the AUTHOR CASCADE (a conditional group
 * rule contributes its children only when its condition holds, which is what `@supports` MEANS), and §7.5's
 * `CSS.supports()` on the `CSS` namespace — a namespace this build does not have, which is exactly why the
 * entry below is declared rather than left static: when that member lands it is THIS call, not a second
 * definition of what "supported" means. `@import ... supports(...)` is the fourth and is the same question
 * again (core/css/css_at_rule_prelude.h parses that function's contents; deciding them is here).
 *
 * VALIDITY AND TRUTH ARE TWO ANSWERS AND THE GRAMMAR IS WHY. §6 says the production is "purposely very loose
 * for forwards-compatibility reasons, since the <general-enclosed> production allows for substantial future
 * extensibility", and then: "Any @supports rule that does not parse according to the grammar above ... is
 * invalid. Style sheets must not use such a rule and processors must ignore such a rule (including all of its
 * contents)." So `@supports display:flex { … }` (no parentheses) is not a false condition — it is not a rule
 * at all, and its contents go with it — while `@supports (frobnicate: 1) { … }` IS a rule whose condition is
 * false and whose contents stay in `cssRules`. One boolean cannot say both, which is why the return value and
 * the out-parameter are two things.
 *
 * `<general-enclosed>` IS FALSE, NOT UNKNOWN, and that is the one place this grammar differs from the media
 * query one it is otherwise a copy of. §6 states the difference outright — the syntax "is similar to that
 * defined for <media-condition> in [MEDIAQUERIES-4], but without the 'unknown' value logic" — and §6's own
 * table gives `<general-enclosed>` the result FALSE. So there is no three-valued logic here and none of
 * core/css/media_query.c's; `not (frobnicate)` is TRUE where the media-query reading would make it unknown.
 *
 * THE GRAMMAR, from §6 (`<general-enclosed>` is Media Queries Level 4 §3 "Syntax"):
 *
 *   <supports-condition> = not <supports-in-parens>
 *                        | <supports-in-parens> [ and <supports-in-parens> ]*
 *                        | <supports-in-parens> [ or <supports-in-parens> ]*
 *   <supports-in-parens> = ( <supports-condition> ) | <supports-feature> | <general-enclosed>
 *   <supports-feature>   = <supports-decl>
 *   <supports-decl>      = ( <declaration> )
 *   <general-enclosed>   = [ <function-token> <any-value>? ) ] | [ ( <any-value>? ) ]
 *
 * `and` AND `or` MAY NOT MIX WITHOUT PARENTHESES, which the three alternatives above state by having no
 * production that admits both, and §6 gives the reason: "to avoid confusion caused by precedence rules, the
 * syntax does not allow and, or, and not operators to be mixed without a layer of parentheses."
 *
 * THE KEYWORDS ARE CHECKED AS IDENT TOKENS AND THAT IS WHAT ENFORCES THE SPACE. §6's tests require `not`,
 * `and` and `or` to be followed by whitespace, and there is no whitespace rule in this file: CSS Syntax
 * tokenizes `not(x)` as a single FUNCTION token, so it never matches the IDENT the `not` arm wants and falls
 * to `<general-enclosed>` — which is FALSE and still a valid rule, exactly as the spec's at-supports-014
 * expects. The tokenizer is the rule; a hand-written space check beside it would be a second one.
 *
 * WHAT "SUPPORTED" IS, §6.1: "A CSS processor is considered to support a declaration (consisting of a property
 * and value) if it accepts that declaration (rather than discarding it as a parse error) within a style rule."
 * That is not a table of property names this file could hold — it is the engine's OWN declaration parser
 * answering about itself, so `css_supports_declaration` runs core/css/css_style_declaration.h's serialization
 * over the text and reads whether anything survived. A property this build parses is supported and one it does
 * not is not, which is the definition rather than an approximation of it, and it moves on its own the day the
 * parser does.
 *
 * IT IS CONCRETE, NOT CONCOLIC, AND THE CONTRAST WITH `@media` IS THE POINT. A media query asks about the
 * ENVIRONMENT — a viewport this headless engine does not have — so core/css/media_query.h answers it with a
 * concolic value and a branch on it forks the alternate-viewport world. A feature query asks about THIS USER
 * AGENT, which is the very program doing the asking: the answer is knowable, so making it concolic would fork
 * a world that cannot exist and would leave `matches` unable to say the one thing it knows for certain. */
#ifndef ENGINE_HOST_BROWSER_CORE_CSS_CSS_SUPPORTS_H
#define ENGINE_HOST_BROWSER_CORE_CSS_CSS_SUPPORTS_H

#include <stdbool.h>
#include <stddef.h>

/* CSS Conditional §6's `<supports-condition>` over `text`, PARSED and EVALUATED in one pass.
 *
 * Returns FALSE when the text matches no production of the grammar — the at-rule is then invalid and CSS
 * Syntax drops it, contents included — and leaves `*matches` untouched. Returns TRUE otherwise, with
 * `*matches` set to §6's own boolean result for the condition.
 *
 * Evaluation does NOT short-circuit, and that is a parsing requirement rather than an oversight: every term of
 * an `and`/`or` chain has to be parsed for the rule to be valid at all, so `(display:none) or (!!!)` must be
 * refused whatever the first term answered. */
bool css_supports_condition(const char *text, size_t len, bool *matches);

/* §6.1's definition of support over ONE declaration's text — `display: flex`, with no surrounding parentheses
 * and no trailing `;`. This is `<supports-decl>`'s leaf and it is also what §7.5's `CSS.supports(property,
 * value)` asks once it has joined its two arguments with a colon.
 *
 * FALSE FOR ANYTHING THAT IS NOT ONE DECLARATION, which is what keeps `<supports-decl>` and
 * `<general-enclosed>` from having to be told apart by the caller: a text holding a top-level `;` is two
 * declarations or one and a fragment, so it is not a `<declaration>` and this answers false — the same answer
 * `<general-enclosed>` carries, which is why the grammar's two arms need no separate result here. */
bool css_supports_declaration(const char *decl, size_t len);

#endif
