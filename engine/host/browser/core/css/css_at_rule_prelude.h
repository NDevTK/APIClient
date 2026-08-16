/* A STATEMENT AT-RULE'S PRELUDE, sliced into the parts its own grammar names — CSS Cascade §2's `@import` and
 * CSS Namespaces §2's `@namespace`.
 *
 * WHY THIS IS A COMPONENT AND NOT A LINE IN THE RULE BUILDER. Lexbor's stylesheet parse hands a statement
 * at-rule over as its at-keyword and the RAW SPAN of its prelude (core/css/css_style_declaration.h's
 * `CssomRule`), because lexbor keeps no parsed form for either of these two: `@import` is not in its at-rule
 * table at all, and its `@namespace` record is `{ uintptr_t reserved; }`. So the grammar that turns
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

#endif
