/* AN AT-RULE'S PRELUDE, sliced into the parts its own grammar names — CSS Cascade §2's `@import`, CSS
 * Namespaces §2's `@namespace`, CSS Paged Media §4.3's `@page` and CSS Animations §3's `@keyframes`.
 *
 * A `<keyframe-block>`'s PRELUDE IS IN HERE TOO, AND IT IS NOT A CATEGORY SLIP. §3 defines `@keyframes` as
 * `@keyframes <keyframes-name> { <qualified-rule-list> }` and then defines `<keyframe-block> =
 * <keyframe-selector># { <declaration-list> }` in the same production, so the keyframe selector list is part
 * of the `@keyframes` AT-RULE'S grammar and exists nowhere else — a `0%, 100%` prelude outside one is not a
 * keyframe selector at all, it is an invalid style rule. It has the same shape as every entry below (a token
 * stream lexbor keeps no parsed form of) and it is read by the same tokenizer, so splitting it out would put
 * one at-rule's grammar in two files.
 *
 * WHY THIS IS A COMPONENT AND NOT A LINE IN THE RULE BUILDER. Lexbor's stylesheet parse hands these at-rules
 * over as their at-keyword and the RAW SPAN of the prelude (core/css/css_style_declaration.h's `CssomRule`),
 * because lexbor keeps no parsed form for any of them: `@import` and `@page` are not in its at-rule table at
 * all, and its `@namespace` record is `{ uintptr_t reserved; }`. So the grammar that turns
 * `url("a.css") supports(x) screen` into an href, a supports condition and a media query list has to exist
 * somewhere, and it is one problem with one set of invariants: it either MATCHES the at-rule's grammar or the
 * rule is INVALID and CSS Syntax drops it. Answering "false" is therefore a real answer with a spec behind it,
 * not a failure — `@namespace { }` and `@import ;` are not rules, and no user agent puts them in `cssRules`.
 *
 * IT TOKENIZES WITH LEXBOR'S OWN CSS TOKENIZER rather than scanning bytes, because the two things this has to
 * get right are exactly the two a byte scan gets wrong: a `<string>` carries CSS escapes (`url('quote\"quote')`
 * is the five-plus-five character URL `quote"quote`, which css/cssom/cssimportrule.html pins byte for byte) and
 * `url(` is an ident-like token whose QUOTED form is a function token followed by a string. `lxb_css_syntax_*`
 * decides both, and is standalone: a tokenizer needs no parser, no arena and no selector state.
 * core/css/media_query.c's own scanner is not a counter-example — its grammar contains no strings and no URLs,
 * which is the reason its note gives for scanning the source directly.
 *
 * THE SPANS THAT ARE NOT TOKENS ARE RAW SOURCE. `supportsText` is "the <supports-condition> declared in the
 * at-rule ITSELF" and `layerName` is "the layer name declared in the at-rule itself" — the spec asks for what
 * the author wrote, and this engine has no parsed form of a supports condition to re-serialize from (CSS
 * Conditional §7.4's CSSSupportsRule is unbuilt). The media query list is different: it is handed on as text to
 * §4.4's create-a-MediaList, which parses and canonicalises it, so `@import url(x) aLL` reads back as `all`. */
#ifndef ENGINE_HOST_BROWSER_CORE_CSS_CSS_AT_RULE_PRELUDE_H
#define ENGINE_HOST_BROWSER_CORE_CSS_CSS_AT_RULE_PRELUDE_H

#include <stdbool.h>
#include <stddef.h>

/* CSS Cascade §2's `@import [ <url> | <string> ] [ layer | layer(<layer-name>) ]? <import-conditions>`, where
   `<import-conditions> = [ supports( [ <supports-condition> | <declaration> ] ) ]? <media-query-list>?`. */
typedef struct {
    /* The `<url>` or `<string>`, unescaped — §6.4.4's `href`, "the URL specified by the @import at-rule", which
       is the SPECIFIED one and not the resolved one ("to get the resolved URL use the href attribute of the
       associated CSS style sheet"). Never NULL: the grammar has no arm without it. OWNED. */
    char *href;
    /* §6.4.4's `layerName`: NULL when the at-rule declares no layer, the EMPTY STRING for the anonymous
       `layer` keyword, and the `<layer-name>` otherwise. The three are three different answers. OWNED. */
    char *layer_name;
    /* §6.4.4's `supportsText`: NULL when the at-rule declares no supports condition, otherwise the raw
       `<supports-condition>` between the function's parentheses. OWNED. */
    char *supports_text;
    /* The `<media-query-list>` tail, as text for §4.4's create-a-MediaList. Never NULL — an absent one is the
       EMPTY media query list, which is the empty string and not the absence of one. OWNED. */
    char *media_text;
} CssImportPrelude;

/* Answers false when the prelude does not match the grammar, in which case NOTHING is allocated. */
bool css_prelude_import(const char *prelude, size_t len, CssImportPrelude *out);
void css_import_prelude_free(CssImportPrelude *p);

/* CSS Namespaces §2's `@namespace <namespace-prefix>? [ <string> | <url> ]`. `*pprefix` is the EMPTY STRING
   when the at-rule declares no prefix — §6.4.9 says `prefix` returns "the prefix of the @namespace at-rule or
   the empty string if there is no prefix", so the default namespace is not a null. Both OWNED; on false
   nothing is allocated. */
bool css_prelude_namespace(const char *prelude, size_t len, char **pprefix, char **puri);

/* CSS Paged Media §4.3's `<page-selector-list> = <page-selector>#`, where
 * `<page-selector> = [ <ident-token>? <pseudo-page>* ]!` and `<pseudo-page> = : [ left | right | first |
 * blank ]`, PARSED AND SERIALIZED IN ONE STEP.
 *
 * ONE STEP BECAUSE THE PARSED FORM HAS NO OTHER CONSUMER. CSSOM §6.4.7 names two algorithms it then declines
 * to define — "Need to define the rules for parse a list of CSS page selectors and serialize a list of CSS
 * page selectors" — and the only thing this engine ever does with the parse is serialize it, because a rule is
 * made of TEXT (core/css/css_rule.h states why: a lexbor pointer has no cross-tier identity, so a rule that
 * held one could neither park nor fork). A parsed form handed out here would be a second representation with
 * one reader.
 *
 * THE GRAMMAR IS WHITESPACE-SENSITIVE, and that is the whole reason it is tokenized rather than split: §4.3
 * says "no whitespace is allowed between the productions in <page-selector> or <pseudo-page> (similar to the
 * rule for <compound-selector>)", so `named:first` is a page selector and `named :first` is not one at all —
 * which is exactly the pair css/cssom/cssom-pagerule.html asserts from both sides. Lexbor's tokenizer emits a
 * WHITESPACE token where there is whitespace, so the presence of one BETWEEN productions is what refuses the
 * selector, while whitespace around the `#` multiplier's commas is still allowed.
 *
 * THE SERIALIZATION follows from the grammar and from §2.1: the page name is SERIALIZED AS AN IDENTIFIER (it
 * is an author-chosen ident, so its case is the author's and its escapes are §2.1's), each pseudo-page is a
 * COLON followed by its keyword ASCII-LOWERCASED (a CSS keyword is ASCII case-insensitive, which is why
 * `named:First` reads back as `named:first`), repeats and order are preserved (§4.4 says "repeated occurrences
 * of the same pseudo-classes are allowed and do increase specificity", so they are not a set), and the
 * selectors are joined by §2.1's serialize-a-comma-separated-list — ", ".
 *
 * OWNED. NULL when the text is not a page selector list at all, which is a rule CSS Syntax drops. The EMPTY
 * STRING is a real answer and not that one: `<page-selector-list>?` is optional in `@page`'s own grammar, so
 * `@page { }` has an empty list, and §6.4.7's setter takes "" as a non-null parse and replaces the list with
 * it. */
char *css_prelude_page_selectors(const char *prelude, size_t len);

/* CSS Animations §3's `<keyframes-name> = <custom-ident> | <string>`, as the NAME it denotes — "the two
 * syntaxes are equivalent in functionality; the name is the value of the ident or string".
 *
 * IT IS NOT SERIALIZED HERE, WHICH IS THE WHOLE DIFFERENCE FROM THE ENTRY ABOVE. A page selector list has one
 * canonical spelling and `selectorText` returns it, so the parse and the serialization are one step; a
 * keyframes name is an author string that CSSOM §6.4's CSSKeyframesRule arm serializes DIFFERENTLY from how
 * §6.3.2's `name` attribute returns it (`name` is `initial`, `cssText` carries `"initial"`), and §6.3.2's
 * setter stores a name this grammar would refuse. So what crosses is the NAME, and the serialization is
 * core/css/css_rule.h's.
 *
 * WHAT IS REFUSED IS TWO SPECIFICATIONS' WORTH AND BOTH HALVES ARE NORMATIVE. CSS Values §4.2: "The CSS-wide
 * keywords are not valid <custom-ident>s. The default keyword is reserved and is also not a valid
 * <custom-ident>. ... Excluded keywords are excluded in all ASCII case permutations." CSS Animations §3 adds
 * "The <custom-ident> additionally excludes the none keyword. The <string> additionally excludes the empty
 * string (but allows the string "none" and other excluded keywords)" — which is why `@keyframes initial {}` is
 * dropped and `@keyframes "initial" {}` is a rule, and why the two arms cannot share one test.
 *
 * OWNED. NULL when the prelude is not a `<keyframes-name>`, which is an at-rule whose grammar failed and which
 * CSS Syntax drops — `@keyframes {}`, `@keyframes none {}` and `@keyframes a b {}` are not rules. The name is
 * FULLY CASE-SENSITIVE ("two names are equal only if they are codepoint-by-codepoint equal"), so it crosses
 * exactly as it was written. */
char *css_prelude_keyframes_name(const char *prelude, size_t len);

/* IS `name` A KEYWORD A `<keyframes-name>` CANNOT SPELL AS AN IDENT — CSS Values §4.2's exclusions from
 * `<custom-ident>` (the CSS-wide keywords, and the reserved `default`) plus CSS Animations §3's `none`,
 * compared in all ASCII case permutations as §4.2 requires ("Excluded keywords are excluded in all ASCII case
 * permutations").
 *
 * ONE ENTRY BECAUSE IT IS ONE SET, ASKED FROM TWO DIRECTIONS. The parse above refuses such a name outright, so
 * `@keyframes initial {}` is not a rule. CSSOM §6.4's CSSKeyframesRule arm QUOTES it instead — "if the
 * attribute is a CSS wide keyword, or the value default, or the value none, then it is serialized as a string.
 * Otherwise, it is serialized as an identifier" — because §6.3.2's setter runs no grammar and a name the parse
 * would have refused can therefore be on a rule. The two are the same three exclusions and must stay the same
 * three: quoting is what makes the serialization RE-PARSE as the rule it came from, which is the property
 * every serialize-a-CSS-rule arm has to have, and two copies could disagree about `revert-layer` or about
 * case and produce a `cssText` that does not. */
bool css_prelude_keyframes_name_excluded(const char *name);

/* CSS Animations §3's `<keyframe-selector># `, where `<keyframe-selector> = from | to | <percentage [0,100]>`,
 * PARSED AND SERIALIZED IN ONE STEP for the reason `css_prelude_page_selectors` is: the only thing this engine
 * ever does with the parse is serialize it back, because a rule is made of TEXT (core/css/css_rule.h).
 *
 * THE SERIALIZATION IS A LIST OF PERCENTAGES AND NOTHING ELSE, which §6.2.2 states as the attribute's own
 * definition rather than leaving to §6.4: "This attribute represents the keyframe selector as a
 * comma-separated list of percentage values. The from and to keywords map to 0% and 100%, respectively." So
 * `from` reads back as `0%`, `to` as `100%`, each number through CSSOM §6.7.2's serialize-a-<number> and the
 * list through §2.1's serialize-a-comma-separated-list — ", ". Order and repeats are the author's: a keyframe
 * selector list is a list of keys and not a set, and `25%, 75%` is one rule that `findRule('75%')` does not
 * match (§6.3.6's own worked example says so).
 *
 * THE RANGE IS PART OF THE GRAMMAR AND SO IS THE UNIT. §3: "Values less than 0% or higher than 100% are
 * invalid and cause their <keyframe-block> to be ignored", with its own note that "the percentage unit
 * specifier must be used on percentage values. Therefore, 0 is an invalid keyframe selector." Both are
 * refusals of the whole block, which is what a NULL here is.
 *
 * OWNED. NULL when the text is not a keyframe selector list, and never the empty string: the `#` multiplier
 * has no zero-length arm, so `{ }` with no prelude at all is not a `<keyframe-block>`. */
char *css_prelude_keyframe_selectors(const char *prelude, size_t len);

#endif
