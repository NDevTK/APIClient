/* WHAT A `@font-feature-values` RULE MAY CONTAIN — CSS Fonts 4 §6.9.1 "Basic syntax"'s closed membership
 * question, which is the fact CSS Fonts 4 §12.2 "The CSSFontFeatureValuesRule interface" is built out of and
 * which nothing else in this engine can answer.
 *
 * IT IS A COMPONENT OF ITS OWN AND NOT A PREDICATE INSIDE core/css/css_rule.c, for the reason
 * core/css/css_page.h gives about `@page`: the question is ABOUT THE CONTENTS of a rule and it is asked from
 * more than one place — core/css/css_rule.c decides which child rules survive the parse, and whichever
 * component later reads a feature value block's declarations has to know which block it is reading — and a copy
 * in each is a copy that can disagree about a name. The PRELUDE is a different problem and belongs with the
 * other prelude grammars, because a prelude is a GRAMMAR over a token stream and this is a table.
 *
 * A CLOSED LIST IS THE SPEC'S OWN SHAPE HERE. CSS Fonts 4 §6.9.1 writes the production out:
 *
 *   <font-feature-value-type> = <@stylistic> | <@historical-forms> | <@styleset> | <@character-variant>
 *                             | <@swash> | <@ornaments> | <@annotation>
 *
 * and says of the enclosing block that its contents are "at-rules named by one of the
 * <font-feature-value-type> at-keyword tokens" or the CSS Fonts 4 §4.9.1 "Controlling Font Display Per
 * Font-Family via @font-feature-values" `font-display` descriptor. Seven names and a descriptor; there is no
 * open tail to wave through.
 *
 * THESE SEVEN ARE SUBSIDIARY AT-RULES AND NOT AT-RULES OF A STYLE SHEET, WHICH IS THE WHOLE OF WHAT THIS
 * ANSWERS. CSS Fonts 4 §6.9.1 names them so in as many words — an `@font-feature-values` prelude is followed
 * by "a block containing multiple feature value blocks, a special type of subsidiary at-rule" — so `@swash { }`
 * written at a style sheet's top level, or inside an `@media`, is an at-rule that is invalid IN THAT CONTEXT,
 * which CSS Syntax 3 §8 "CSS stylesheets" DISCARDS. It is exactly the shape CSS Paged Media §4.3's margin
 * at-rules have (core/css/css_page.h's own entry says the same thing from the other side), and it is why the
 * seven are dropped rather than crashed on: a crash there would name a CSSOM interface to build for a rule no
 * user agent represents in that position.
 *
 * AND THE DROP IS NOT A FALLBACK FOR AN UNKNOWN AT-RULE. Every one of the seven is in CSS Syntax 3 §8's
 * recognized-at-rule registry (a CSS specification DEFINES it), so the registry answers yes and must: what is
 * being decided here is the CONTEXT half of CSS Syntax 3 §8's sentence and not the RECOGNITION half, and the
 * two are separate questions with separate answers. Inside a `@font-feature-values` block the very same seven
 * names are the rule's own content — CSS Fonts 4 §12.2 makes each one a `CSSFontFeatureValuesMap` attribute of
 * the enclosing rule rather than a child rule of it — so the enclosing rule's builder consumes them, and this
 * predicate is what stops them being read as rules anywhere else.
 *
 * WHAT IS DELIBERATELY NOT HERE YET. CSS Fonts 4 §6.9.1's per-block VALUE GRAMMAR (`<font-feature-index>` for
 * `@annotation`, `@ornaments`, `@stylistic` and `@swash`; `@character-variant`'s pair; `@styleset`'s
 * repetition) and CSS Fonts 4 §6.9.2 "Multi-valued feature value definitions"' limits are the material
 * CSS Fonts 4 §12.2's maps are filled from, and nothing reads them until that interface exists — a table with
 * no consumer is the producer-with-no-reader shape CLAUDE.md names, where the unread half is free to be wrong
 * for as long as nobody misses it. They arrive with their first consumer. */
#ifndef ENGINE_HOST_BROWSER_CORE_CSS_CSS_FONT_FEATURE_VALUES_H
#define ENGINE_HOST_BROWSER_CORE_CSS_CSS_FONT_FEATURE_VALUES_H

#include <stdbool.h>

/* CSS Fonts 4 §6.9.1 "Basic syntax"'s seven `<font-feature-value-type>` at-rules (`@stylistic`,
   `@historical-forms`, `@styleset`, `@character-variant`, `@swash`, `@ornaments`, `@annotation`), by the name
   the parse reports: the at-rule's identifier with NO `@`, ASCII-lowercased. `false` for every other
   at-keyword.
   INSIDE a `@font-feature-values` block a true answer means the at-rule is that rule's own content, which
   CSS Fonts 4 §12.2 "The CSSFontFeatureValuesRule interface" represents as a map attribute and not as a child
   rule; ANYWHERE ELSE it means the at-rule is invalid in its context and CSS Syntax 3 §8 "CSS stylesheets"
   discards it. A false answer says only that this at-keyword is not one of the seven — whether it is a rule at
   all is the recognized-at-rule registry's question and not this one's. */
bool css_font_feature_value_at_rule(const char *name);

#endif
