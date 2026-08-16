/* CSS Cascade §7's DEFAULTING step. See css_defaulting.h for why it is a component and why it decides rather
   than fetches. */
#include <ctype.h>
#include <stdbool.h>
#include <string.h>

#include "check.h"
#include "core/css/css_defaulting.h"

/* An ASCII case-insensitive comparison against a keyword, over a value with the surrounding whitespace lexbor's
   serialization may leave on it. CSS Syntax makes a keyword ASCII case-insensitive, so `INHERIT` and `inherit`
   are one value and are not two rows. */
static bool css_def_keyword_is(const char *v, const char *kw)
{
    size_t n, k = strlen(kw), j;

    while (*v && isspace((unsigned char)*v)) v++;
    for (n = strlen(v); n > 0 && isspace((unsigned char)v[n - 1]); n--) { }
    if (n != k) return false;
    for (j = 0; j < k; j++)
        if ((char)tolower((unsigned char)v[j]) != kw[j]) return false;
    return true;
}

/* §7.3's CSS-WIDE KEYWORDS, in the order the section defines them — css-cascade-5 §7.3.1 through §7.3.6, whose
   own opening sentence names the last three together ("the keywords revert, revert-layer, and revert-rule are
   cascade-dependent keywords; some contexts may restrict their use while allowing the other CSS-wide
   keywords").
   §7.3.6's `revert-rule` USED TO BE OFF THIS LIST, on the stated ground that lexbor's grammar does not admit it
   so no declaration carrying one can reach a cascade. It went back on because the set has a second consumer
   that does not go through lexbor's property grammar at all: CSS Cascade §6.4.2 refuses a `<layer-name>`
   segment that is a CSS-wide keyword, and core/css/css_at_rule_prelude.c tokenizes that prelude itself — so
   `@layer revert-rule { }` reaches the question and the missing row would have answered it wrong.
   THAT GROUND IS NOW GONE FROM THE OTHER DIRECTION TOO, and the reason is worth keeping because it is what
   makes the set uniform: §7.3 says "all CSS properties can accept these values", and lexbor's value grammar
   predates §7.3.5 and §7.3.6, so `height: revert-layer` failed that grammar and arrived as CSS Syntax's
   INVALID DECLARATION. core/css/css_style_declaration.c now reads an invalid declaration whose raw value is one
   of these six as the declaration it is, so all six reach a cascade for EVERY property rather than only for the
   ones no grammar in this build types. (The old note also cited css-cascade-6, which has no §7.3 at all; the
   keyword is css-cascade-5's.)
   EACH ROW CARRIES WHICH KIND IT IS, so the "is this a CSS-wide keyword" question and the "which roll-back is
   this" question are ONE list read twice rather than two lists that can disagree — which is the failure this
   very row has already had once, in the other direction, when `revert-rule` was on one reading and off the
   other. §7.3.1 through §7.3.3 are answerable from the property alone and are CSS_ROLLBACK_NONE here; the
   remaining three are §7.3's cascade-dependent keywords, and what each rolls back is the enumeration's whole
   content. */
static const struct { const char *name; CssRollback rollback; } CSS_WIDE[] = {
    { "inherit",      CSS_ROLLBACK_NONE },     /* §7.3.2 */
    { "initial",      CSS_ROLLBACK_NONE },     /* §7.3.1 */
    { "unset",        CSS_ROLLBACK_NONE },     /* §7.3.3 */
    { "revert",       CSS_ROLLBACK_ORIGIN },   /* §7.3.4 — "rolls back the cascade to the ... earlier origin" */
    { "revert-layer", CSS_ROLLBACK_LAYER },    /* §7.3.5 — "except it works by cascade layer" */
    { "revert-rule",  CSS_ROLLBACK_RULE },     /* §7.3.6 — "except it works by style rule" */
};

bool css_wide_keyword(const char *v)
{
    unsigned i;

    DCHECK(v != NULL, "CSS Cascade §7.3's CSS-wide keyword question was asked about a NULL value");
    for (i = 0; i < sizeof(CSS_WIDE) / sizeof(CSS_WIDE[0]); i++)
        if (css_def_keyword_is(v, CSS_WIDE[i].name)) return true;
    return false;
}

CssRollback css_rollback_keyword(const char *v)
{
    unsigned i;

    DCHECK(v != NULL, "CSS Cascade §7.3's cascade-dependent-keyword question was asked about a NULL value");
    for (i = 0; i < sizeof(CSS_WIDE) / sizeof(CSS_WIDE[0]); i++)
        if (css_def_keyword_is(v, CSS_WIDE[i].name)) return CSS_WIDE[i].rollback;
    return CSS_ROLLBACK_NONE;
}

/* §7.2's `Inherited: yes` LONGHANDS, transcribed from the property definition tables that state it. See the
   header for why this list is the enumerable half and why a name outside it is answered by §7.2's own sentence
   rather than by a default standing in for a missing row. */
static const char *const CSS_INHERITED[] = {
    /* CSS Color 4, CSS UI, css-color-adjust */
    "caret-color", "caret-shape", "color", "color-scheme", "cursor", "forced-color-adjust",
    "print-color-adjust", "pointer-events", "scrollbar-color", "visibility",
    /* CSS Fonts 4 — every longhand of the `font` and `font-variant` shorthands */
    "font-family", "font-feature-settings", "font-kerning", "font-language-override", "font-optical-sizing",
    "font-palette", "font-size", "font-size-adjust", "font-stretch", "font-style",
    "font-synthesis-position", "font-synthesis-small-caps", "font-synthesis-style", "font-synthesis-weight",
    "font-variant-alternates", "font-variant-caps", "font-variant-east-asian", "font-variant-emoji",
    "font-variant-ligatures", "font-variant-numeric", "font-variant-position", "font-variation-settings",
    "font-weight",
    /* CSS Text 4 and CSS Text Decoration's inherited members (the `text-decoration-*` longhands are NOT
       inherited — §2 propagates a decoration to descendant boxes without inheriting the property) */
    "hyphenate-character", "hyphenate-limit-chars", "hyphens", "letter-spacing", "line-break", "line-height",
    "overflow-wrap", "tab-size", "text-align", "text-align-all", "text-align-last", "text-autospace",
    "text-combine-upright", "text-emphasis-color", "text-emphasis-position", "text-emphasis-style",
    "text-indent", "text-justify", "text-rendering", "text-shadow", "text-size-adjust", "text-spacing-trim",
    "text-transform", "text-underline-offset", "text-underline-position", "text-wrap", "text-wrap-mode",
    "text-wrap-style", "white-space", "white-space-collapse", "word-break", "word-spacing", "word-wrap",
    /* CSS Writing Modes 4 (`unicode-bidi` is NOT inherited) and CSS Ruby */
    "direction", "ruby-align", "ruby-merge", "ruby-position", "text-orientation", "writing-mode",
    /* CSS Lists 3, CSS Tables, CSS Fragmentation's widows/orphans, CSS Generated Content's quotes */
    "border-collapse", "border-spacing", "caption-side", "empty-cells", "list-style-image",
    "list-style-position", "list-style-type", "orphans", "quotes", "widows",
    /* CSS Images 3 */
    "image-orientation", "image-rendering",
    /* MathML Core §The math-* properties */
    "math-depth", "math-shift", "math-style",
    /* SVG 2's painting and text properties, which are CSS properties and reach this cascade the moment a page
       reads one back */
    "clip-rule", "color-interpolation", "color-interpolation-filters", "fill", "fill-opacity", "fill-rule",
    "marker-end", "marker-mid", "marker-start", "paint-order", "shape-rendering", "stroke", "stroke-dasharray",
    "stroke-dashoffset", "stroke-linecap", "stroke-linejoin", "stroke-miterlimit", "stroke-opacity",
    "stroke-width", "text-anchor",
    /* CSS 2.1 §A's AURAL properties, which have no rendering here and still have a computed value a page can
       read back */
    "azimuth", "elevation", "pitch", "pitch-range", "richness", "speak", "speak-header", "speak-numeral",
    "speak-punctuation", "speech-rate", "stress", "voice-family", "volume",
};

bool css_property_inherited(const char *name)
{
    unsigned i;

    DCHECK(name != NULL, "CSS Cascade §7.2's `Inherited:` line was asked for with no property name");
    /* css-variables §2: a CUSTOM PROPERTY is "an ordinary property, so it can be declared on any element, is
       resolved with the normal inheritance and cascade rules" — the whole family inherits, and it is named by
       its two-dash prefix rather than enumerated, since the set is the author's and not a spec's. */
    if (name[0] == '-' && name[1] == '-') return true;
    for (i = 0; i < sizeof(CSS_INHERITED) / sizeof(CSS_INHERITED[0]); i++)
        if (strcmp(CSS_INHERITED[i], name) == 0) return true;
    return false;
}

CssDefaulting css_defaulting_of(const char *name, const char *cascaded)
{
    DCHECK(name != NULL, "CSS Cascade §7's defaulting step was asked about no property");
    /* §7.1 and §7.2's shared condition, "unless the cascade results in a value" — which is the state almost
       every property on almost every element is in, and the whole reason inheritance exists. */
    if (cascaded == NULL)
        return css_property_inherited(name) ? CSS_DEFAULTING_INHERITED : CSS_DEFAULTING_INITIAL;
    if (!css_wide_keyword(cascaded)) return CSS_DEFAULTING_DECLARED;
    /* §7.3.2: "If the cascaded value of a property is the inherit keyword, the property's specified and
       computed values are the inherited value." */
    if (css_def_keyword_is(cascaded, "inherit")) return CSS_DEFAULTING_INHERITED;
    /* §7.3.1: "If the cascaded value of a property is the initial keyword, the property's specified value is
       its initial value." */
    if (css_def_keyword_is(cascaded, "initial")) return CSS_DEFAULTING_INITIAL;
    /* §7.3.3: "if it is an inherited property, this is treated as inherit, and if it is not, this is treated
       as initial" — which is why §7.2's line is read here and not only above. */
    if (css_def_keyword_is(cascaded, "unset"))
        return css_property_inherited(name) ? CSS_DEFAULTING_INHERITED : CSS_DEFAULTING_INITIAL;
    DCHECK(css_rollback_keyword(cascaded) != CSS_ROLLBACK_NONE,
           "a CSS-wide keyword reached §7's defaulting that is none of the six — CSS_WIDE and this switch are "
           "one list and have come apart");
    /* §7.3.4's `revert`, §7.3.5's `revert-layer` and §7.3.6's `revert-rule` ARE NOT DEFAULTING, and that is
       §7.3's own classification rather than a division of labour invented here: they are the CASCADE-DEPENDENT
       keywords, each defined by a fact about the declaration's place in the cascade — the origin it belongs to,
       the cascade layer it is in, the style rule it was written in — and none of those three facts is carried
       by a cascaded value. So core/css/css_cascade.h discharges all three, by re-running §6.1's sort with that
       origin, that layer's level range or that rule removed, and hands this step the value the cascade would
       have answered without them. §7.3.4's one arm that produces a keyword rather than a value — "user-agent
       origin: Equivalent to unset" — is answered as `unset` and resolved above.
       So a roll-back arriving here is the cascade returning a keyword it was supposed to have discharged, and
       there is nothing this component could compute in its place: the origin is gone. */
    DFAIL("a §7.3 CASCADE-DEPENDENT keyword reached §7's DEFAULTING step. `revert`, `revert-layer` and "
          "`revert-rule` are resolved by the cascade that knows the declaration's origin, layer and rule "
          "(core/css/css_cascade.c's `css_cascade_value`), so `cssom_cascaded_value` never answers one — find "
          "the caller that built a cascaded value without going through it");
    return CSS_DEFAULTING_INITIAL;
}
