/* AN AT-RULE'S PRELUDE, sliced into the parts its own grammar names — CSS Cascade §2's `@import`, CSS
 * Namespaces §2's `@namespace` and CSS Paged Media §4.3's `@page`.
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

#endif
