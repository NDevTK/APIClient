/* css-fonts-4 §2.1 "Font family: the font-family property" — THE `font-family` VALUE: ONE PARSE PRODUCING A
 * LIST, AND ONE SERIALIZER OVER THAT LIST.
 *
 * WHY IT IS ITS OWN COMPONENT AND NOT A PREDICATE INSIDE core/css/css_font_shorthand.c, WHICH IS WHERE §2.1.1
 * USED TO LIVE. That file owns §2.7's SEQUENCE, and the §2.1.1 predicate it carried answered ONE question —
 * is this text a `<'font-family'>` — for ONE caller, the `font` shorthand's last term. The LONGHAND asks a
 * different and strictly larger question, because CSSOM does not merely accept or reject a declaration: it
 * READS ONE BACK, so `font-family` needs the value in a form that can be re-emitted and re-parsed. A validity
 * predicate cannot produce that, and a serializer written beside it over the ORIGINAL TEXT cannot either —
 * `'21st Century'` and `"21st Century"` are the same value spelled two ways and exactly one of them is what a
 * page reads back. So the parse produces the LIST §2.1's `#` describes and the serializer is stated over that
 * list, which is the only arrangement in which the two agree by construction.
 *
 * THE LIST IS NOT EXPORTED, AND THAT IS A DECISION RATHER THAN AN OMISSION. Nothing outside this file consumes
 * a `<font-family-name>` yet: the cascade carries a specified value as TEXT (core/css/css_computed_value.h), and
 * font selection does not exist. A list handed out with no reader is a producer with no consumer — the shape
 * CLAUDE.md names, where the unread half is free to be wrong for as long as nobody misses it — so the list is
 * internal and the entry below answers the one question both callers actually ask. WHEN A FONT-MATCHING
 * COMPONENT EXISTS it takes the list; until then the parse and the serializer are one call.
 *
 * WHAT §2.1.1 STATES AND WHAT IT DOES NOT. `<font-family-name> = <string> | <custom-ident>+`, and "Font family
 * names other than generic families or system font families must either be given quoted as `<string>`s, or
 * unquoted as a sequence of one or more `<custom-ident>`". It states the EXCLUSION — "Any identifier which
 * could be misinterpreted as a pre-defined keyword in the font-family value definition, or the CSS-wide
 * keywords, is not allowed" — and it states the JOIN — "If a sequence of identifiers is given as a
 * `<font-family-name>`, the computed value is the name converted to a string by joining all the identifiers in
 * the sequence by single spaces". WHAT IS IN NEITHER css-fonts-4 NOR CSSOM IS THE CHOICE BETWEEN THE TWO
 * SPELLINGS ON THE WAY BACK OUT: CSSOM §2.1 "Common Serializing Idioms" gives serialize a string and serialize
 * an identifier and says nothing about which a `<family-name>` takes. This file's answer is stated at the
 * serializer, and it is pinned by the corpus rather than by a sentence — `css/css-fonts/parsing/
 * font-family-valid.html` asserts `"New Century Schoolbook", serif` reads back UNQUOTED and `"inherit",
 * "serif"` reads back QUOTED, which is the whole rule in two examples.
 *
 * THE EXCLUSION IS PER IDENTIFIER AND NOT PER LIST ITEM, AND THE DIFFERENCE IS A TEST. §2.1.1's own invalid
 * example `font-family: cursive serif` is a TWO-identifier `<custom-ident>+` in which neither identifier is a
 * CSS-wide keyword, so an exclusion asked of the JOINED name (`cursive serif`) admits it and the corpus says
 * a browser does not. §2.1.1's normative sentence is about "Any identifier", and its closing paragraph says
 * why: "UAs must not consider these keywords as matching the `<font-family-name>` type."
 *
 * A SINGLE IDENTIFIER THAT IS A GENERIC KEYWORD IS THE `<generic-font-family>` ARM AND NOT AN EXCLUDED
 * `<custom-ident>`, which is why the arm is chosen before the exclusion is asked. §2.1's value is
 * `[ <font-family-name> | <generic-font-family> ]#` and `<generic-font-family>` is the alternative that
 * matches `serif`; the exclusion exists to stop the OTHER alternative claiming it. Asking the exclusion first
 * would make `font-family: serif` invalid.
 *
 * THE TWO PRODUCTION NAMES IN THE PARAGRAPH ABOVE READ `<family-name>` AND `<generic-family>` UNTIL THEY WERE
 * FETCHED, and the correction is recorded rather than made silently because the REASONING they sit in was
 * exactly right and only the spellings were stale — which is the routine half of CLAUDE.md's
 * mis-transcription rule and is established by this file's OTHER statements of the same equation, which spell
 * both productions correctly. css-fonts-4 §2.1 "Font family: the font-family property" states
 * `Value: [ <font-family-name> | <generic-font-family> ]#` and §2.1.1 is titled "Syntax of
 * <font-family-name>"; there is no `<family-name>` production in css-fonts-4 at all. It matters here because
 * the DESCRIPTOR entry below is stated as the same production with the list and the generic arm removed, and
 * a reader comparing two spellings of one production cannot see that.
 *
 * SYSTEM FONT NAMES ARE NOT EXCLUDED HERE, AND THE REASON IS THE SENTENCE'S OWN SCOPE. §2.1.1's note mentions
 * them — "if you really have a font whose name is the same as one of the `<generic-font-family>` names, or the
 * system font names, or the CSS-wide keywords, it must be quoted" — and the NORMATIVE sentence above it is
 * narrower: "misinterpreted as a pre-defined keyword IN THE FONT-FAMILY VALUE DEFINITION". §2.1.3's
 * `<system-font-family-name>` is in §2.7's value definition and not in §2.1's — §2.7 says "System fonts can
 * only be set as a whole" — so `font-family: caption` is a `<custom-ident>` family named `caption`, and only a
 * `font: caption` declaration reads that word as a system font. */
#ifndef ENGINE_HOST_BROWSER_CORE_CSS_CSS_FONT_FAMILY_H
#define ENGINE_HOST_BROWSER_CORE_CSS_CSS_FONT_FAMILY_H

/* The SPECIFIED value of a `font-family` declaration whose value is `value`, CSSOM-serialized — the answer
   CSSOM §6.6.1 "The CSSStyleDeclaration Interface"'s getPropertyValue reads back, and therefore the text the
   round-trip in the corpus re-parses. NULL for a value outside css-fonts-4 §2.1's grammar, which is CSS
   Syntax's INVALID DECLARATION and is dropped whole.
   A CSS-WIDE KEYWORD IS ANSWERED ASCII-LOWERCASED AND NEVER PARSED AS A FAMILY, because CSS Cascade 5 §7.3
   "Explicit Defaulting" makes it a value for every property ahead of that property's own grammar; §7's
   DEFAULTING step is what resolves it (core/css/css_computed_value.h says where).
   OWNED: the caller frees. */
char *css_font_family_value(const char *value);

/* css-fonts-4 §4.2 "Font family: the font-family descriptor" — the `@font-face` DESCRIPTOR of that name,
 * whose fetched `Value:` line is `<font-family-name>` and nothing else. NULL for a value outside it, which is
 * CSS Syntax's INVALID DECLARATION and is dropped whole; otherwise the CSSOM serialization, OWNED.
 *
 * IT IS THE ENTRY ABOVE MINUS TWO THINGS, AND BOTH ARE READ OFF THE FETCHED `Value:` LINES RATHER THAN
 * ARGUED. §2.1's PROPERTY is `[ <font-family-name> | <generic-font-family> ]#`; §4.2's DESCRIPTOR is
 * `<font-family-name>`. So the descriptor takes ONE name where the property takes a list, and it has no
 * `<generic-font-family>` ALTERNATIVE at all.
 *
 * AN UNQUOTED GENERIC IS THEREFORE INVALID HERE, AND THAT IS A SENTENCE ABOUT THE TYPE RATHER THAN ABOUT THE
 * VALUE DEFINITION IT SITS IN — which is the whole reason it can be decided without inventing anything.
 * §2.1.1's exclusion reads "Any identifier which could be misinterpreted as a pre-defined keyword in the
 * font-family value definition", and read alone that is scoped to a value definition the descriptor does not
 * have. Its closing paragraph is not: "Font family names that happen to be the same as a font-family keyword
 * value (e.g. CSS-wide keywords such as inherit, or <generic-font-family> keywords such as serif) must be
 * quoted to prevent confusion with the keywords of the same names. UAs must not consider these keywords as
 * matching the <font-family-name> type." A keyword that does not match the TYPE does not match it wherever
 * the type appears, so `@font-face { font-family: serif }` declares nothing and
 * `@font-face { font-family: "serif" }` declares the family named `serif` — the `<string>` arm, which the
 * same paragraph is telling authors to reach for.
 *
 * NO CSS-WIDE KEYWORD ARM, AND THE TWO ANSWERS ARE OPPOSITE SO IT IS STATED RATHER THAN LEFT OUT. The entry
 * above takes CSS Cascade 5 §7.3 "Explicit Defaulting"'s keywords ahead of §2.1's own grammar because that
 * section says of them "all CSS properties can accept these values". A descriptor is not a property — core/css/css_style_declaration.h says so where
 * it explains why a descriptor maps to no longhand — so §7.3 does not reach it, and an unquoted `inherit`
 * falls into the same §2.1.1 sentence quoted above and is refused. Running the §7.3 arm here would have
 * ACCEPTED it and answered "inherit", which is why this is a decision and not an omission.
 *
 * ITS SECOND CALLER IS CSS Font Loading 3 §2.1 "The Constructor", WHICH DOES NOT EXIST YET. That algorithm's
 * step 2 is "Parse the family argument, and the members of the descriptors argument, according to the grammars
 * of the corresponding descriptors of the CSS @font-face rule ... set font face's corresponding attributes to
 * the serialization of the parsed values" — this question, for the `family` argument, with this answer's own
 * NULL as that step's "fail to parse correctly". It is named here so the next diff routes to this entry
 * instead of writing a second one beside it. */
char *css_font_family_descriptor_value(const char *value);

#endif
