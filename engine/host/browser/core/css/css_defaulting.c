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

/* §7.3's CSS-WIDE KEYWORDS, in the order the section defines them. `revert-rule` (css-cascade-6 §7.3.6) is NOT
   here: lexbor's grammar does not admit it, so a declaration carrying one never reaches a cascade in this
   engine and a row for it would describe a value nothing can produce. */
static const char *const CSS_WIDE[] = { "inherit", "initial", "unset", "revert", "revert-layer" };

bool css_wide_keyword(const char *v)
{
    unsigned i;

    DCHECK(v != NULL, "CSS Cascade §7.3's CSS-wide keyword question was asked about a NULL value");
    for (i = 0; i < sizeof(CSS_WIDE) / sizeof(CSS_WIDE[0]); i++)
        if (css_def_keyword_is(v, CSS_WIDE[i])) return true;
    return false;
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
    DCHECK(css_def_keyword_is(cascaded, "revert") || css_def_keyword_is(cascaded, "revert-layer"),
           "a CSS-wide keyword reached §7's defaulting that is none of the five — CSS_WIDE and this switch are "
           "one list and have come apart");
    DFAIL("CSS Cascade §7.3.4's `revert` and §7.3.5's `revert-layer` roll the cascade BACK — to the cascaded "
          "value of the earlier ORIGIN, and to the earlier cascade LAYER — so the specified value is what the "
          "cascade would have answered had the current origin's (or the current layer's) declarations not been "
          "there at all. The cascade in css_style_declaration.c cannot be asked that question: it walks its "
          "layers in order and returns the first that answers, keeping no record of WHICH origin the winner "
          "came from and no way to be re-run with an origin excluded — and `revert` is defined by the origin "
          "of the DECLARATION carrying it, not of the element. BUILD the cascade as a list of matched "
          "declarations tagged with their origin and layer, resolved by one ordering pass, so both keywords "
          "are the same pass run with a CEILING; note that §7.3.4 makes `revert` in the user-agent origin "
          "equivalent to `unset`, which is the base case that terminates the roll-back");
    return CSS_DEFAULTING_INITIAL;
}
