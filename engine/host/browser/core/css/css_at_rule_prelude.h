/* AN AT-RULE'S OWN GRAMMAR, sliced into the parts it names — CSS Cascade §2's `@import`, CSS Namespaces §2's
 * `@namespace`, CSS Paged Media §4.3's `@page`, CSS Animations §3's `@keyframes`, CSS Cascade §6.4.4's `@layer`
 * and CSS Properties and Values API 1 §3's `@property`. That is almost always the PRELUDE, because a body is
 * either rules or declarations and lexbor parses both; §3's `@property` is the one at-rule whose body is
 * neither, and its section at the bottom says why its descriptors are read here too.
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

/* CSS Cascade §6.4.2's `<layer-name> = <ident> [ '.' <ident> ]*`, as the LIST the two `@layer` at-rules take,
 * PARSED AND SERIALIZED IN ONE STEP for the reason `css_prelude_page_selectors` is: the only thing this engine
 * ever does with the parse is serialize it back, because a rule is made of TEXT (core/css/css_rule.h).
 *
 * ONE ENTRY FOR TWO AT-RULES BECAUSE IT IS ONE PRODUCTION UNDER TWO MULTIPLIERS. §6.4.4.1's block at-rule is
 * `@layer <layer-name>? { <rule-list> }` and §6.4.4.2's statement at-rule is `@layer <layer-name>#;`, so what a
 * NAME is does not differ at all and only HOW MANY the rule may declare does — which is the CALLER's question,
 * because it is exactly what tells the two INTERFACES apart. `n` is therefore a real answer at every value, and
 * ZERO is the interesting one: it is §6.4.2.1's anonymous layer ("when a @layer rule omits its <layer-name> ...
 * its layer name gains a unique anonymous segment; it therefore cannot be referenced from the outside"), which
 * `?` admits and `#` does not.
 *
 * THE GRAMMAR IS WHITESPACE-SENSITIVE INSIDE A NAME AND NOT BETWEEN THEM, which is the whole reason it is
 * tokenized rather than split on `.` and `,`: §6.4.2 calls a layer name "a period-separated list of <ident>
 * tokens with no intervening white space", so `a.b` is one name and `a . b` is not a name at all, while the `#`
 * multiplier's commas may carry whitespace as every other one's may. It is the same distinction CSS Paged Media
 * §4.3 draws inside a `<page-selector>` and lexbor's tokenizer answers it the same way, by emitting a
 * WHITESPACE token where there is whitespace.
 *
 * WHAT IS REFUSED IS §6.4.2's OWN SENTENCE and no more: "The CSS-wide keywords are reserved for future use, and
 * cause the rule to be invalid at parse time if used as an <ident> in the <layer-name>." That set is
 * core/css/css_defaulting.h's, asked and never restated. It is NOT the `<keyframes-name>` set beside it, and
 * the difference is the production: `<layer-name>` is `<ident>` where `<keyframes-name>` is `<custom-ident>`,
 * so CSS Values §4.2's reserved `default` is a perfectly good layer name and so is `none`.
 *
 * EACH NAME CROSSES SERIALIZED — every segment through CSSOM §2.1's serialize-an-identifier, joined by `.`.
 * §6.4.2 gives a layer name its identity by its segments ("layer names represent the same cascade layer if they
 * contain the same segments in the same order"), so the escapes are the only thing to normalise and the case
 * stays the author's. Serializing here rather than at the rule is also what keeps `a\.b` — ONE segment
 * containing a period — distinguishable from `a.b`, which a raw join could not be; and it is what makes
 * `cssText` re-parse as the rule it came from, the property every serialize-a-CSS-rule arm has to have.
 *
 * Answers false when the prelude is not a `<layer-name>#` at all, in which case NOTHING is allocated — an
 * at-rule whose grammar failed, which CSS Syntax drops. */
typedef struct {
    char   **v;   /* each `<layer-name>`, serialized. OWNED. */
    unsigned n;   /* how many — ZERO is `@layer { }`'s anonymous layer and not a failure */
} CssLayerNames;

bool css_prelude_layer_names(const char *prelude, size_t len, CssLayerNames *out);
void css_layer_names_free(CssLayerNames *p);

/* ONE serialized `<layer-name>` back into its SEGMENTS — `reset.type` into `reset` and `type`, `a\.b` into the
   single segment `a\.b`. Never fails and never answers zero segments: the empty name is not one this component
   ever produces, and §6.4.2.1's anonymous layer is the ABSENCE of a name rather than an empty one.
   IT IS HERE BECAUSE THE JOIN IS. §6.4.2 gives a layer name its identity by its segments ("layer names
   represent the same cascade layer if they contain the same segments in the same order"), so CSS Cascade
   §6.4.3's order (core/css/css_layer_order.h) has to compare them one segment at a time — and the convention
   that decides where one segment ends is the escaping this file writes when it joins them. Two files reading
   that convention could disagree about `a\.b`, and one of them would be wrong about which cascade layer a rule
   is in. OWNED through `css_layer_names_free`. */
void css_layer_name_segments(const char *name, CssLayerNames *out);

/* CSS Conditional Rules 5 §5.4 "Container Queries: the @container rule"'s WHOLE prelude —
 *
 *   @container <container-condition># { <rule-list> }
 *   <container-condition> = [ <container-name>? <container-query>? ]!
 *   <container-name>      = <custom-ident>
 *   <container-query>     = not <query-in-parens>
 *                         | <query-in-parens> [ [ and <query-in-parens> ]* | [ or <query-in-parens> ]* ]
 *   <query-in-parens>     = ( <container-query> ) | ( <size-feature> ) | style( <style-query> )
 *                         | scroll-state( <scroll-state-query> ) | <general-enclosed>
 *
 * TWO STRINGS PER CONDITION, BECAUSE §9.1 "The CSSContainerRule interface" ASKS FOR EXACTLY TWO. Its
 * `conditions` is a `FrozenArray<CSSContainerCondition>` over the dictionary `{ name, query }`, built "with
 * name set to the serialized <container-name> of condition if specified, or "" otherwise, and query set to the
 * <container-query> specified in condition". `containerName`, `containerQuery` and §7.2's `conditionText` are
 * then all defined in terms of that list, so this parse produces the list and nothing derives a second one.
 *
 * THE NAME IS SERIALIZED AND THE QUERY IS RAW, WHICH IS THE SAME SPLIT `@layer` AND `@supports` ALREADY MAKE.
 * A `<container-name>` is a `<custom-ident>` this engine has a canonical spelling for — CSSOM §2.1's
 * serialize-an-identifier, exactly as a `<layer-name>` segment is — so it crosses serialized and `cssText`
 * re-parses. A `<container-query>` crosses as the AUTHOR'S RAW SPAN, trimmed, because §9.1 forbids a round
 * trip through this engine's understanding of it: the query must be carried "without any logical
 * simplifications, so that the returned query will evaluate to the same result as the specified query in any
 * conformant implementation of this specification (INCLUDING IMPLEMENTATIONS THAT IMPLEMENT FUTURE EXTENSIONS
 * ALLOWED BY THE <general-enclosed> EXTENSIBILITY MECHANISM)". That is CSS Conditional §7.4's sentence for
 * `@supports` word for word, and it has the same consequence: re-serializing from a parsed form would delete
 * precisely the constructs `<general-enclosed>` exists to carry.
 *
 * THE STRUCTURE IS CHECKED AND THE FEATURES ARE NOT, AND THE GRAMMAR IS WHY RATHER THAN CONVENIENCE. Every arm
 * of `<query-in-parens>` is a BALANCED GROUP at the token level — a `(`-block or a `name(`-block — and the last
 * arm, `<general-enclosed> = [ <function-token> <any-value>? ) ] | [ ( <any-value>? ) ]`, accepts ANY of them.
 * So a group this build cannot read is not a parse failure, it is the extensibility arm doing its job, and
 * §5.4 says what it then means: "As with media queries, <general-enclosed> evaluates to unknown." Descending
 * into a group to validate a `<size-feature>` would therefore REJECT rules the standard requires to be kept —
 * the identical reasoning core/css/css_supports.h gives for `<supports-in-parens>`, which is why these two
 * grammars are checked the same way and neither carries a table of feature names.
 * What IS structural, and is enforced: `and` and `or` MAY NOT MIX without a layer of parentheses (the three
 * alternatives of `<container-query>` have no production admitting both), and the `not` arm takes exactly ONE
 * `<query-in-parens>` and nothing after it. The keywords are matched as IDENT tokens, which is what enforces
 * the space with no space rule written here: CSS Syntax tokenizes `not(x)` as a single FUNCTION token, so it
 * never reaches the `not` arm and falls to `<general-enclosed>` — a valid query that is unknown.
 *
 * `!` IS THE ONE THING THAT MAKES AN EMPTY CONDITION A FAILURE. Both terms are optional individually and the
 * group is not, so `@container { }` and the empty entry in `@container a, , b` match no production — an
 * at-rule whose grammar failed, which CSS Syntax §8 "CSS stylesheets" drops with its contents.
 *
 * Answers false when the prelude is not a `<container-condition>#`, in which case NOTHING is allocated. Never
 * answers zero conditions: the `#` multiplier has no zero-length arm. */
typedef struct {
    /* §9.1's `name`: the `<container-name>` SERIALIZED, or the EMPTY STRING when the condition declares none —
       which is §9.1's own "or "" otherwise" and not an absence this record has to distinguish from it. Never
       NULL. OWNED. */
    char *name;
    /* §9.1's `query`: the `<container-query>` as the author wrote it, trimmed, or the EMPTY STRING when the
       condition declares none (a bare `<container-name>`, which §5.4 admits — "if the <container-query> is
       omitted, the query container is eligible as long as the <container-name> matches"). Never NULL. OWNED. */
    char *query;
} CssContainerCondition;

typedef struct {
    CssContainerCondition *v;
    unsigned               n;   /* at least one on a match — the `#` multiplier has no zero-length arm */
} CssContainerConditions;

bool css_prelude_container_conditions(const char *prelude, size_t len, CssContainerConditions *out);
void css_container_conditions_free(CssContainerConditions *p);

/* ---- CSS Properties and Values API 1 §3 "The @property Rule" ------------------------------------------------
 *
 * ITS DESCRIPTORS ARE IN HERE BESIDE ITS PRELUDE, AND THAT IS THE `<keyframe-block>` DECISION AGAIN. §3 gives
 * the at-rule as `@property <custom-property-name># { <declaration-list> }` and then defines that declaration
 * list's three descriptors — §3.1's `syntax`, §3.2's `inherits`, §3.3's `initial-value` — in the same section,
 * and they exist NOWHERE ELSE: `syntax: "<length>"` written anywhere but inside an `@property` is a declaration
 * of a property no specification defines. So this is ONE at-rule's grammar, and splitting the prelude from the
 * body would put it in two files for the sake of a word in this file's title.
 * They arrive here as TEXT lexbor kept no parsed form of, which is this component's whole reason for existing:
 * an unknown property name becomes CSS Syntax's custom declaration and its value survives as the raw token
 * stream, so the grammar that turns `"<length>"` into the six-character syntax `<length>` has to be somewhere,
 * and it either MATCHES the descriptor's own grammar or the descriptor is invalid and ignored (§3: "unknown
 * descriptors are invalid and ignored, but do not invalidate the @property rule" — and §3.1's and §3.2's own
 * sentences say the same of a descriptor whose VALUE does not match).
 *
 * CSS Variables 1 §2 "Defining Custom Properties: the --* family of properties" is what a `<custom-property-name>`
 * is: "the <custom-property-name> production ... it's defined as any <dashed-ident> (a valid identifier that
 * starts with two dashes), except -- itself, which is reserved for future use by CSS", where CSS Values 4 §4.3
 * "Prefixed Author-defined Identifiers: the <dashed-ident> type" makes a `<dashed-ident>` "a <custom-ident>,
 * with all the case-sensitivity that implies, with the additional restriction that it must start with two
 * dashes". FULLY CASE-SENSITIVE, so a name crosses exactly as it was written — and UNESCAPED, because a token's
 * value is what identifies a custom property (`var(--fo\6f)` reads `--foo`), which is also why the SERIALIZATION
 * back to `@property --foo` is CSSOM §2.1's serialize-an-identifier at the rule and not a copy of the source.
 *
 * THE `#` MULTIPLIER IS §3's AND HAS NO ZERO-LENGTH ARM, so `@property { }` is an at-rule whose grammar failed
 * and CSS Syntax drops it — `n` is one or more on every true answer, and a rule with several names is a rule
 * §3 calls valid ("a valid @property rule represents a custom property registration for each
 * <custom-property-name> in the rule's prelude"). What §6.1's CSSOM makes of several is not this file's
 * question and is not settled anywhere: see core/css/css_rule.h.
 *
 * Answers false when the prelude is not a `<custom-property-name>#`, in which case NOTHING is allocated. */
typedef struct {
    char   **v;   /* each `<custom-property-name>`, unescaped and verbatim. OWNED. */
    unsigned n;   /* how many — never zero on a true answer */
} CssPropertyNames;

bool css_prelude_property_names(const char *prelude, size_t len, CssPropertyNames *out);
void css_property_names_free(CssPropertyNames *p);

/* §3.1's `syntax` descriptor — `Value: <string>`, so the descriptor's declared value must be ONE `<string>` and
 * nothing else, and what it denotes is that string's own VALUE: `syntax: "I\\ dent|none"` declares the syntax
 * `I\ dent|none`, one backslash. §6.1's `syntax` is "the syntax associated with the @property, EXACTLY AS
 * SPECIFIED", which is why nothing here trims it — `" <color># "` is the syntax ` <color># `, spaces included,
 * and §5.4.2's own step 1 is what strips them when the string is PARSED rather than when it is read back.
 *
 * OWNED. NULL when the declared value is not a single `<string>`, which is a descriptor whose grammar failed —
 * §3's "unknown descriptors are invalid and ignored, but do not invalidate the @property rule" is the rule the
 * caller then applies, so a NULL here is the descriptor's ABSENCE and never the rule's. */
char *css_property_descriptor_syntax(const char *value, size_t len);

/* §3.2's `inherits` descriptor — `Value: true | false`, two CSS keywords and therefore ASCII case-insensitive,
 * so `inherits: TRUE` is the true arm. Answers false when the declared value is neither, which is again a
 * descriptor that is ignored rather than a rule that is invalid; `*pinherits` is written only on true. */
bool css_property_descriptor_inherits(const char *value, size_t len, bool *pinherits);

#endif
