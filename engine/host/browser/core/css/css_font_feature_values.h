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
 * WHAT ARRIVED WITH ITS FIRST CONSUMER. This entry's own banner used to end by saying that CSS Fonts 4 §6.9.1's
 * per-block VALUE GRAMMAR and CSS Fonts 4 §6.9.2 "Multi-valued feature value definitions"' limits were
 * deliberately absent because nothing read them — a table with no consumer being the producer-with-no-reader
 * shape CLAUDE.md names. CSS Fonts 4 §12.2's interface below IS that consumer, so the grammar, the limits and
 * the block parse are here: `css_font_feature_values_max_values` is the count half and
 * `css_font_feature_values_map_fill` is the walk. It is recorded rather than deleted because the REASON is the
 * durable half — a member added here with no reader would be free to be wrong for as long as nobody missed it,
 * which is exactly the state the two of them were kept out of. */
#ifndef ENGINE_HOST_BROWSER_CORE_CSS_CSS_FONT_FEATURE_VALUES_H
#define ENGINE_HOST_BROWSER_CORE_CSS_CSS_FONT_FEATURE_VALUES_H

#include <stdbool.h>

#include "quickjs.h"

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

/* HOW MANY VALUES ONE OF THE SEVEN ADMITS PER DECLARATION — css-fonts-4 §6.9.1 "Basic syntax"'s per-block value
   grammar, as the ONE number css-fonts-4 §12.2 "The CSSFontFeatureValuesRule interface"'s `set` and this
   component's block parse both read. 0 means UNBOUNDED and is a real answer rather than "unknown": §6.9.1
   states a grammar for five of the seven and states none for `@historical-forms`, and `@styleset`'s carries a
   `+`.
   §12.2'S `InvalidAccessError` IS ABOUT THIS COUNT AND NOTHING ELSE — "the set() method throws an
   InvalidAccessError exception when the input sequence to set() contains more than the limited number of
   values" — so the RANGE halves of §6.9.1's productions are not asked of a `set` at all, and must not be:
   css-fonts-4 §6.9.2 "Multi-valued feature value definitions" says in as many words that "values greater than
   99 or equal to 0 do not generate a syntax error when parsed but enable no OpenType features", which
   contradicts §6.9.1's own `[0,20]` on `@styleset` inside one document. §6.9.2 is the section that states what
   the numbers MEAN, and `css/cssom/CSSFontFeatureValuesRule.html` pins it: `di: 10 9 4 5` parses and
   `set("di", 43)` stores 43.
   `name` is the at-rule's identifier with NO `@`, ASCII-lowercased — the same spelling the predicate above
   takes. Asking it about a name that is not one of the seven is a caller error and aborts. */
unsigned css_font_feature_values_max_values(const char *name);

/* css-fonts-4 §12.2's `CSSFontFeatureValuesMap` for ONE of the seven feature value blocks — `kind` being that
 * block's at-keyword without the `@`, which is what decides the count limit above. OWNED: the caller frees.
 *
 * THE MAP ENTRIES ARE A JS ARRAY OF `[name, valuesArray]` PAIRS IN AN OWN PRIVATE-SYMBOL SLOT, which is
 * core/css/media_list.c's arrangement and is chosen for its two reasons. (1) Every member §12.2 declares
 * MUTATES — `set`, `delete` and `clear` are exactly the writes two flows must be able to disagree about — and
 * an Array's mutations are property writes the per-flow COW delta already captures, where a malloc'd list would
 * be captured as a POINTER and leave the nodes reachable from nothing on a context switch. (2) The store must
 * PARK to the IDB cold tier and resume, which a JS value does for free. So there is no class record, no COW
 * layout, no finalizer and no gc_mark here, and therefore no write site left to miss.
 * INSERTION ORDER IS THE MAP'S ORDER AND THAT IS THE TYPE'S OWN WORD. Web IDL §2.5.11 "Maplike declarations":
 * "objects implementing an interface that is declared to be maplike represent an ordered map of key-value
 * pairs, initially empty, known as that object's map entries". An ORDERED map is what §3.7.11.2's iterator and
 * §3.7.11.6's `forEach` both walk, so an Array is the shape rather than an approximation of one. */
JSValue css_font_feature_values_map_new(JSContext *ctx, const char *kind);

/* Is `v` a `CSSFontFeatureValuesMap`? The brand — an own-slot read, for a caller holding something it took off
   an attribute. */
bool css_font_feature_values_map_is(JSContext *ctx, JSValueConst v);

/* css-fonts-4 §6.9.1's FEATURE VALUE DECLARATIONS of ONE block, merged into `map`. `block` is that block's
   serialized declaration list — the text core/css/css_style_declaration.h's CSSOM_BLOCK_FEATURE_VALUES
   context produces, which is the page's own name and value spellings and nothing judged — and may be NULL or
   empty for a block that declares nothing.
   IT MERGES RATHER THAN REPLACES, because §6.9.1 says the same block type may appear more than once:
   "Specifying the same <font-feature-value-type> more than once is valid; their contents are cascaded
   together", and "If the same tuple appears more than once in a document … the last-defined one is used". So a
   second `@swash` adds to the first and a repeated name overwrites.
   A DECLARATION OUTSIDE §6.9.1's GRAMMAR IS DROPPED AND THE REST OF THE BLOCK SURVIVES, which is that section's
   own sentence: "A syntax error within a font feature value declaration makes the declaration invalid and
   ignored, but does not invalidate the font feature value block it occurs in." */
void css_font_feature_values_map_fill(JSContext *ctx, JSValueConst map, const char *block);

/* ONE feature value block's declarations, serialized back — `@<kind> { <name>: <v> <v>; … }`, or NULL for a map
   that holds nothing. For css-fonts-4 §12.2's rule's own `cssText`, which is built out of the seven maps rather
   than out of a stored block text precisely because the maps are MUTABLE: a `set` must show through. OWNED. */
char *css_font_feature_values_map_serialize(JSContext *ctx, JSValueConst map);

void css_font_feature_values_init(JSContext *ctx);
/* §12.2's `CSSFontFeatureValuesMap.prototype` for ONE realm — declared into core/realm.h's list. */
void css_font_feature_values_install_proto(JSContext *ctx);
/* `CSSFontFeatureValuesMap` as a global. */
void css_font_feature_values_install(JSContext *ctx, JSValueConst global);
void css_font_feature_values_free(JSRuntime *rt);

#endif
