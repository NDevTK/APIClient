/* css-fonts-4 §4.3 "Font reference: the src descriptor" — THE `@font-face` DESCRIPTOR'S VALUE GRAMMAR, PARSED
 * AS A LIST AND SERIALIZED BACK OVER THAT LIST.
 *
 * WHY IT IS ITS OWN COMPONENT. §4.3's `Value:` line is `<font-src-list>` and §4.3.1 "Parsing the src
 * descriptor" opens "To parse a <font-src-list> production, parse a list of <font-src>s" — so this is a
 * production with a section of its own, and §4.3.1's own first sentence about it is that "Parsing this
 * descriptor is more complicated than parsing other descriptors". It is not a predicate: CSSOM reads a
 * descriptor BACK, so what a caller needs is the value in a form that re-parses to itself, which is the same
 * argument core/css/css_font_family.h makes for the `font-family` longhand one descriptor over.
 *
 * WHAT WAS THERE BEFORE, BECAUSE THE DIFFERENCE IS NOT "MORE VALIDATION" BUT "A VALUE AT ALL". lexbor's
 * registry has no `src` — it is not a property — so a `src` declaration reached CSSOM as an unknown name whose
 * RAW TEXT was stored verbatim, and `css_shorthand_validates_longhand("src")` answering false is what kept it
 * there. Verbatim is a defensible answer for a descriptor nothing reads; it stops being one the moment a
 * consumer has to resolve urls out of it, which is what CSS Font Loading §2.2 "The load() method" does with
 * the `[[Urls]]` slot core/fonts/font_face.c fills.
 *
 * The parsed list has one reader: css-fonts-4 §4.3.3 "Selecting items in the src", asked by
 * core/css/css_font_selector.h when a face is loaded. It takes the items from css_font_src_items below, over
 * the same parse the serialization is built from, so the two cannot disagree about one declaration.
 *
 * NAMED RESIDUAL — NO ITEM IS FILTERED BY WHAT THIS ENGINE SUPPORTS, AND THE STANDARD PERMITS THAT ARM.
 *   WHAT IS NOT COVERED: half of §4.3.1's sentence "If a component value is parsed correctly and is of a font
 *     format or font tech that the UA supports, add it to the list of supported sources. If parsing a
 *     component value results in a parsing error or its format or tech are unsupported, do not add it to the
 *     list of supported sources." The PARSING-ERROR half is built here and decides every item; the
 *     UNSUPPORTED half is not asked at all, so an item naming a format or a tech this engine could never
 *     activate stays in the list.
 *   WHY IT IS NOT ANSWERED RATHER THAN ANSWERED NARROWLY: "supports" is a claim about font ACTIVATION, which
 *     §4.3 describes as "downloading the file or reading it from disk, parsing it, and perhaps additional
 *     user-agent-dependent steps" — and this engine performs none of it. An empty supported set would make
 *     `src: url(f.woff2) format(woff2)` a parse error, and a full one would claim a decoder this engine does
 *     not have; both invent a capability. §4.3.3 "Selecting items in the src"'s own requirement is about
 *     DOWNLOADING — "conformant user agents must skip downloading a font resource if the format hint
 *     indicates an unsupported or unknown font format, or if any of the font technologies are unsupported by
 *     the user agent" — so it belongs with the diff that downloads one.
 *   WHAT THE NEXT DIFF BUILDS: the filter, in the component that activates a font, reading that component's
 *     own format and tech capabilities rather than a list written here.
 *   HOW ITS ABSENCE WOULD SHOW: a `src` reads back an item whose `format()` or `tech()` names something the
 *     engine cannot activate, so the count of items in the serialization is the count the grammar admitted
 *     and never the count that could be loaded.
 *
 * NAMED RESIDUAL — `local()` INHERITS core/css/css_font_family.c's PER-IDENTIFIER EXCLUSION, WHICH THE
 * CONFORMANCE CORPUS CONTRADICTS FOR A MULTI-IDENTIFIER NAME.
 *   WHAT IS NOT COVERED: css-values-4 §4.2 "Unprefixed Author-defined Identifiers: the <custom-ident> type"
 *     says "The CSS-wide keywords are not valid <custom-ident>s. The default keyword is reserved and is also
 *     not a valid <custom-ident>", and css_font_family.c applies that PER IDENTIFIER of a `<custom-ident>+`
 *     — which is the reading css-fonts-4 §2.1.1 "Syntax of <font-family-name>"'s own invalid example
 *     `font-family: cursive serif` forces for the GENERIC keywords. For the CSS-wide keywords the corpus says
 *     otherwise — `css/css-fonts/parsing/font-face-src-local.html` asserts `local(default)` INVALID and
 *     `local(default A)` VALID, and three more rows of the same shape — and the disagreement is only
 *     reachable through `local()`, because css-fonts-4 §4.2's descriptor has no such row in the corpus.
 *   WHAT THE NEXT DIFF BUILDS: the split, in core/css/css_font_family.c, between the exclusion that applies to
 *     a SINGLE-identifier name and the one that applies to every identifier of a sequence — which is a
 *     `font-family` question and is not smuggled into a `src` diff, because it moves the longhand's answer too
 *     and that answer has an oracle of its own.
 *   HOW ITS ABSENCE WOULD SHOW: a `src` whose only item is a `local()` naming a font whose FIRST identifier is
 *     a CSS-wide keyword and which has further identifiers after it reads back as undeclared.
 *
 * NAMED RESIDUAL — A `<url-modifier>` IS ACCEPTED AND THEN DROPPED FROM THE SERIALIZATION.
 *   WHAT IS NOT COVERED: css-values-4 §4.5's `<url()> = url( <string> <url-modifier>* ) | <url-token>`, whose
 *     modifier tail this file accepts WHOLE — core/css/css_image.c's reading of §4.5.3 "URL Modifiers", which
 *     says "This specification does not define any <url-modifier>s, but other specs may do so" and makes one
 *     "either an <ident> or a functional notation" — and then keeps only the `<string>`. So the tail is
 *     neither validated against those two shapes nor carried, which is wrong in BOTH directions at once: a
 *     tail that is neither an ident nor a functional notation is admitted, and a tail that IS one is lost.
 *   WHAT THE NEXT DIFF BUILDS: the two shapes §4.5.3 names, read into the item beside its url and emitted
 *     after it — in core/css/, over the `<url>` production, so this file and core/css/css_image.c stop
 *     holding two readings of one type.
 *   HOW ITS ABSENCE WOULD SHOW: a `src` written with anything at all after the string inside a `url()` reads
 *     back with everything after the string gone, and a `src` written with something there that is neither an
 *     ident nor a functional notation reads back as a declaration rather than being dropped.
 *
 * THE BASE URL IS AN OPEN ISSUE IN THE STANDARD AND IS NOT ONE HERE, WHICH IS WHY NOTHING BELOW RESOLVES ONE.
 * css-values-4 §4.5.1 "Relative URLs" is what a stylesheet's urls resolve against, and CSS Font Loading §2.1
 * "The Constructor" carries an ISSUE saying the other caller has no answer yet: "Need to define the base url,
 * so relative urls can resolve. Should it be the url of the document? Is that correct for workers too, or
 * should they use their worker url? Is that always defined?" So this component answers the GRAMMAR and holds
 * the url as the page spelled it; resolution is the fetching component's, at the point it has a base to
 * resolve against, and inventing one here would put one answer in the file least able to know which caller it
 * is serving. */
#ifndef ENGINE_HOST_BROWSER_CORE_CSS_CSS_FONT_SRC_H
#define ENGINE_HOST_BROWSER_CORE_CSS_CSS_FONT_SRC_H

#include <stdbool.h>
#include <stddef.h>

/* One `<font-src>` of §4.3.1: a `<url>` with its optional `format()` and `tech()`, or a `local()` name.
   Exactly one of `url` and `local` is non-NULL. */
typedef struct {
    char *url;              /* the decoded `<url>`, UTF-8, owned. NULL for a `local()` item. */
    char *local;            /* the serialized `<font-family-name>`, owned. NULL for a `<url>` item. */
    const char *format_kw;  /* §4.3.1's `<font-format>` keyword, lower case, or NULL. Not owned. */
    char *format_str;       /* the decoded `<string>` arm of `<font-format>`, owned, or NULL. */
    const char **tech;      /* §4.3.1's `<font-tech>` keywords, lower case, in the page's order. The array
                               is owned; the keywords are not. */
    size_t ntech, captech;
} CssFontSrcItem;

/* §4.3.1's list in the page's order. It grows: the list has no upper bound. */
typedef struct {
    CssFontSrcItem *v;
    size_t n, cap;
} CssFontSrcList;

/* The SPECIFIED value of a `src` descriptor whose value is `value`, CSSOM-serialized — the answer CSSOM
   §6.6.1 "The CSSStyleDeclaration Interface"'s getPropertyValue reads back for an `@font-face` rule, and
   therefore the text the block's own re-parse sees next. NULL is §4.3.1's "If there are no supported entries
   at the end of this process, the value for the src descriptor is a parse error", which is CSS Syntax's
   INVALID DECLARATION and is dropped whole.
   THERE IS NO CSS Cascade 5 §7.3 "Explicit Defaulting" ARM, for the reason core/css/css_font_family.h states
   at the `font-family` DESCRIPTOR: §7.3's keywords are a value for every PROPERTY and a descriptor is not
   one, so `src: inherit` falls into this grammar like any other value and is refused by it.
   OWNED: the caller frees. */
char *css_font_src_descriptor_value(const char *value);

/* §4.3.1's list out of a `src` value into the empty `out`, each item parsed independently and an invalid one
   dropped. False, with `out` left empty, when no item is valid or the value still carries an arbitrary
   substitution function. Release with css_font_src_items_free. */
bool css_font_src_items(const char *value, CssFontSrcList *out);
void css_font_src_items_free(CssFontSrcList *l);

#endif
