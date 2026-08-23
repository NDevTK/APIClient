/* CSS Nesting Module Level 1 §3 "Nesting Style Rules", §3.1 "Syntax", §4 "Nesting Selector: the & selector"
 * and §6 "CSSOM" — the nesting selector, as the two text operations a nested style rule is made of.
 *
 * IT IS TEXT BECAUSE A RULE IS TEXT, and that is core/css/css_rule.h's decision rather than this file's: a
 * lexbor selector lives in an arena and is named by a pointer with no cross-tier identity, so a rule stores the
 * SERIALIZATION and the selector matcher re-parses it. A nested rule's selector is therefore resolved the same
 * way — as text, into text lexbor can parse — and the two halves of §3 fall out as two functions over a span.
 *
 * TWO OPERATIONS AND THEY RUN AT DIFFERENT MOMENTS, which is the whole shape of the component:
 *
 *  ABSOLUTIZE runs ONCE, when the rule is created, and its output is what §6.4.3's `selectorText` answers.
 *  §6 states it outright: "When serializing a relative selector in a nested style rule, the selector must be
 *  absolutized, with the implied nesting selector inserted" — with §6's own example, "the selector > .foo will
 *  serialize as & > .foo". So a nested rule's stored prelude ALWAYS contains the nesting selector, whether the
 *  author wrote one or not, and every later reader has one shape to handle instead of three.
 *
 *  RESOLVE runs at every CASCADE READ, because what it resolves AGAINST can change under it: the parent rule's
 *  `selectorText` is settable (§6.4.3), so `&` names a different set of elements after a page writes one. §4
 *  gives the resolution: "The nesting selector can be desugared by replacing it with the parent style rule's
 *  selector, wrapped in an :is() selector", with `a, b { & c { … } }` equivalent to `:is(a, b) c`.
 *
 * `:is()` IS NOT A CONVENIENCE — IT IS THE SPECIFICITY. §4: "The specificity of the nesting selector is equal
 * to the largest specificity among the complex selectors in the parent style rule's selector list (identical to
 * the behavior of :is())", and Selectors 4 §15 "Calculating a selector's specificity" defines that behaviour
 * ("The specificity of an :is(), :not(), or :has() pseudo-class is replaced by the specificity of the most
 * specific complex selector in its selector list argument"). So desugaring to `:is(parent)` does not merely
 * MATCH the same elements as `&` — it also carries §4's specificity through a selector engine that knows
 * nothing about nesting, because the engine already implements `:is()`. Concatenating the parent's text
 * without the `:is()` would match identically and CASCADE DIFFERENTLY, which §4's own worked example is about
 * (`#a, b { & c { … } }` beats `.foo c`, where the flattened `b c` would lose).
 *
 * IT IS ALSO WHY `&` CANNOT REPRESENT A PSEUDO-ELEMENT AND NOTHING HERE HAS TO SAY SO. §4: "The nesting
 * selector cannot represent pseudo-elements (identical to the behavior of the :is() pseudo-class)", and
 * Selectors 4 §4.2 "The Matches-Any Pseudo-class: :is()" makes its argument a `<forgiving-selector-list>`, in
 * which a pseudo-element is invalid and is dropped. So `.foo, .foo::before { &:hover { … } }` resolves to
 * `:is(.foo, .foo::before):hover`, which matches exactly `.foo:hover` — §4's own stated answer — and the rule
 * that produces it is the selector engine's, not a special case here.
 *
 * WHAT COUNTS AS THE NESTING SELECTOR IS A TOKEN QUESTION, NOT A BYTE ONE. §3.1: "A selector is said to
 * contain the nesting selector if, when it was parsed as any type of selector, a <delim-token> with the value
 * '&' (U+0026 AMPERSAND) was encountered." So an ampersand inside a string (`[title="a & b"]`), inside a
 * comment, or written as the escape `\&` (which is an <ident-token>'s code point, not a delim) is NOT one —
 * and the scan below walks CSS Syntax's string, comment and escape forms for exactly that reason. §3.1's note
 * is also why the scan looks at EVERY depth rather than the top level only: `:is(:unknown(&), .bar)` contains
 * the nesting selector, "so as to catch cases like" that one, where an unknown functional selector is the only
 * part that carries it.
 *
 * WHAT IS RELATIVE AND WHAT IS NOT, §3.1, in the order the spec states it: "A nested style rule accepts a
 * <relative-selector-list> as its prelude (rather than just a <selector-list>). Any relative selectors are
 * relative to the elements represented by the nesting selector", and then "If a selector in the
 * <relative-selector-list> does not start with a combinator but does contain the nesting selector, it is
 * interpreted as a non-relative selector." The combinator test comes FIRST, so `> &` is relative and
 * absolutizes to `& > &`. Selectors 4 §3.4 "Relative Selectors" supplies the implied half — "Relative
 * selectors begin with a combinator, with a selector representing the anchor element implied at the start of
 * the selector. (If no combinator is present, the descendant combinator is implied.)" — so both arms insert
 * the SAME `& `, and the descendant combinator is the space that insertion already carries.
 *
 * THE COMBINATORS ARE THREE CHARACTERS AND THE GRAMMAR IS WHERE THAT COMES FROM. Selectors 4 §16 "Grammar"
 * gives `<combinator> = '>' | '+' | '~'`; §14 "Combinators" adds the descendant combinator, which is
 * whitespace and therefore not a leading character a trimmed selector can start with. The column combinator is
 * not in that production and is not tested for.
 *
 * NO DEPTH BOUND EXISTS OR MAY BE ADDED. Nesting is arbitrarily deep — §3.3 "Nesting Other At-Rules" nests
 * group rules inside style rules inside group rules without limit — and the resolution is applied one level at
 * a time by the walk that owns the rule tree, so this file never sees the depth at all. */
#ifndef ENGINE_HOST_BROWSER_CORE_CSS_CSS_NESTING_H
#define ENGINE_HOST_BROWSER_CORE_CSS_CSS_NESTING_H

#include <stdbool.h>
#include <stddef.h>

/* §3.1's "contains the nesting selector" over one selector's text: is there a `&` <delim-token> anywhere in it,
   at any nesting depth, outside strings, comments and escapes. */
bool css_nesting_contains(const char *sel, size_t len);

/* §3.1's `<relative-selector-list>` SHAPE: does this prelude have it — does it contain the nesting selector,
   or does any of its complex selectors begin with one of Selectors 4 §16's combinators?
   IT EXISTS BECAUSE THE ENGINE'S SELECTOR PARSER KNOWS NEITHER. A `<selector-list>` parser refuses `&:hover`
   and `> .baz` exactly as it refuses `!!!`, so without this question a nested rule the author wrote and a
   piece of garbage are the same event and the rule silently vanishes from `cssRules` with the page's styles
   quietly wrong. It is a SHAPE and not a validity test: `&!!!` has the shape and is not a selector, and §3.1's
   "An invalid nested style rule is ignored, along with its contents" is discharged where every other selector's
   validity is — by the parse the cascade runs over the resolved text. */
bool css_nesting_is_relative(const char *sel, size_t len);

/* §6's ABSOLUTIZE: the `<relative-selector-list>` a nested style rule's prelude is, rewritten so that every
   complex selector in it contains the nesting selector explicitly. OWNED: the caller frees. Never NULL.
   The result is what §6.4.3's `selectorText` answers for a nested rule, and it is what `css_nesting_resolve`
   consumes — the two are one shape on purpose, so that no later reader has to ask whether the author wrote a
   `&` or the parser implied one. */
char *css_nesting_absolutize(const char *sel, size_t len);

/* §4's DESUGARING of one absolutized nested selector against its parent style rule's selector list: every
   nesting selector in `sel` replaced by `:is(<parent>)`. OWNED: the caller frees. Never NULL.
   `parent` is the parent style rule's selector list AS THAT RULE WOULD BE EMITTED — already resolved if the
   parent is itself nested, which is what makes `.a { .b { .c { } } }` come out as `:is(:is(.a) .b) .c` with
   the specificity the flattened `.a .b .c` has. */
char *css_nesting_resolve(const char *sel, size_t len, const char *parent, size_t parent_len);

#endif
