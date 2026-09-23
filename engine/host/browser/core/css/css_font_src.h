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
 * THE LIST IS NOT EXPORTED, AND THAT IS A DECISION WITH A NAMED READER RATHER THAN AN OMISSION — the same one
 * core/css/css_font_family.h makes and for the same reason. Nothing in this engine consumes a `<font-src>`
 * yet: no CSS url is fetched anywhere (`engine_pending_resource_url`'s callers are both ELEMENTS — an
 * `<img>` and a `<link>` — rather than values), so a list handed out today would be a producer with no
 * consumer, free to be wrong for as long as nobody missed it. The reader is named below.
 *
 * NAMED RESIDUAL — THE PARSED LIST IS INTERNAL AND A CONSUMER TAKES THE SERIALIZATION.
 *   WHAT IS NOT COVERED: an entry answering the ITEMS — each item's url, its `format()` and its `tech()` list
 *     as separate values — which CSS Font Loading §2.2's "Using the value of font face's [[Urls]] slot,
 *     attempt to load a font as defined in [CSS-FONTS-3], as if it was the value of a @font-face rule's src
 *     descriptor" needs, and which css-fonts-4 §4.3.2 "Loading an individual item in the src descriptor" is
 *     stated over.
 *   WHAT THE NEXT DIFF BUILDS: that entry, here, over the list this file already builds — never a second
 *     parse at the consumer, which would be two grammars over one descriptor free to disagree about the same
 *     bytes on the same page.
 *   HOW ITS ABSENCE WOULD SHOW: a component that has to name the resources a `src` refers to can only obtain
 *     them by re-parsing a string, so the same declaration is parsed twice in one document load and the two
 *     answers are separately maintainable.
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

#endif
