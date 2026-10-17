/* css-fonts-4 §2.5.1's absolute-size table and the number `medium` is. See font_size_functions.h for why the
   default font size is a PICKED environment fact, why the `<relative-size>` ratio is not one, and where §2.5.1's
   readability floor lives. */
#include <ctype.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "check.h"
#include "core/css/css_length.h"
#include "core/css/font_size_functions.h"

/* THE ONE PICKED NUMBER, IN CSS PIXELS — what §2.5's `Initial: medium` computes to on this user agent. §2.5.1
   makes every other entry an exact ratio to it, so this is the single point the whole table moves with, and it
   is the only value in this file a reader has to weigh rather than read off the spec. */
#define CSS_DEFAULT_FONT_SIZE_PX 16.0

double css_default_font_size_px(void)
{
    return CSS_DEFAULT_FONT_SIZE_PX;
}

/* Real Chrome's default font size for fixed-width text: Chromium 0842759f
   third_party/blink/public/common/web_preferences/web_preferences.h's `default_fixed_font_size = 13`, beside
   its `default_font_size = 16`. Like the other it is a reader's preference and not a CSS rule. */
#define CSS_DEFAULT_FIXED_FONT_SIZE_PX 13.0

/* Real Chrome's table of font sizes, which is this user agent's: §2.5 makes an `<absolute-size>` "an entry in a
   table of font sizes computed and kept by the user agent", and §2.5.1 lets it "fine-tune these values".
   Chromium 0842759f third_party/blink/renderer/core/css/font_size_functions.cc's
   FontSizeFunctions::FontSizeForKeyword reads the row of kStrictFontSizeTable (kQuirksFontSizeTable in a quirks
   document) for a `medium` of a whole 9 to 16 pixels, and otherwise scales `medium` by kFontSizeFactors, no
   smaller than the minimum logical font size (web_preferences.h's `minimum_logical_font_size = 6`). Rows are
   `medium` 9 to 16, columns xx-small to xxx-large; [0] is the strict table and [1] the quirks one. */
#define CSS_FS_TABLE_MIN 9
#define CSS_FS_TABLE_MAX 16
#define CSS_FS_MIN_LOGICAL_PX 6.0
static const int CSS_FONT_SIZE_TABLE[2][CSS_FS_TABLE_MAX - CSS_FS_TABLE_MIN + 1][8] = {
    { { 9, 9, 9, 9, 11, 14, 18, 27 },   { 9, 9, 9, 10, 12, 15, 20, 30 },  { 9, 9, 10, 11, 13, 17, 22, 33 },
      { 9, 9, 10, 12, 14, 18, 24, 36 }, { 9, 10, 12, 13, 16, 20, 26, 39 }, { 9, 10, 12, 14, 17, 21, 28, 42 },
      { 9, 10, 13, 15, 18, 23, 30, 45 }, { 9, 10, 13, 16, 18, 24, 32, 48 } },
    { { 9, 9, 9, 9, 11, 14, 18, 28 },   { 9, 9, 9, 10, 12, 15, 20, 31 },  { 9, 9, 9, 11, 13, 17, 22, 34 },
      { 9, 9, 10, 12, 14, 18, 24, 37 }, { 9, 9, 10, 13, 16, 20, 26, 40 }, { 9, 9, 11, 14, 17, 21, 28, 42 },
      { 9, 10, 12, 15, 17, 23, 30, 45 }, { 9, 10, 13, 16, 18, 24, 32, 48 } },
};
static const double CSS_FONT_SIZE_FACTORS[8] = { 0.60, 0.75, 0.89, 1.0, 1.2, 1.5, 2.0, 3.0 };
static const CssKeyword CSS_FONT_SIZE_KEYWORDS[8] = {
    CSS_KEYWORD_XX_SMALL, CSS_KEYWORD_X_SMALL, CSS_KEYWORD_SMALL,    CSS_KEYWORD_MEDIUM,
    CSS_KEYWORD_LARGE,    CSS_KEYWORD_X_LARGE, CSS_KEYWORD_XX_LARGE, CSS_KEYWORD_XXX_LARGE,
};
/* Each entry is a function of `medium`, so it is an atom over it, named by its definition. */
static const char *const CSS_FONT_SIZE_OP[2][8] = {
    { "Blink FontSizeForKeyword xx-small", "Blink FontSizeForKeyword x-small", "Blink FontSizeForKeyword small",
      "Blink FontSizeForKeyword medium", "Blink FontSizeForKeyword large", "Blink FontSizeForKeyword x-large",
      "Blink FontSizeForKeyword xx-large", "Blink FontSizeForKeyword xxx-large" },
    { "Blink FontSizeForKeyword quirks xx-small", "Blink FontSizeForKeyword quirks x-small",
      "Blink FontSizeForKeyword quirks small", "Blink FontSizeForKeyword quirks medium",
      "Blink FontSizeForKeyword quirks large", "Blink FontSizeForKeyword quirks x-large",
      "Blink FontSizeForKeyword quirks xx-large", "Blink FontSizeForKeyword quirks xxx-large" },
};

/* §2.5's `<relative-size>` ratio. §2.5's own sentence is the range and the assertion below is over it: "the
   specific ratio is unspecified, but should be around 1.2–1.5". The LOW END is the pick, and the nearest thing
   the spec offers to a reason is one section along and about a DIFFERENT number, so it is quoted as what it is
   rather than as a rule for this one: §2.5.1's note records that "in CSS1, the suggested scaling factor
   between adjacent indexes was 1.5, which user experience proved to be too large" and that CSS2's 1.2 "still
   created issues for the small sizes" — that is the ABSOLUTE-SIZE table's step, which §2.5.1 answered by
   making the factors vary per index. That answer is not available to a `<relative-size>`, because the parent's
   computed value does not say which index it came from (see font_size_functions.h), so what is left is one
   constant inside §2.5's own range, and the end of the range CSS1 was faulted for is not it. */
#define CSS_RELATIVE_SIZE_RATIO 1.2
#define CSS_RELATIVE_SIZE_MIN   1.2
#define CSS_RELATIVE_SIZE_MAX   1.5

CssPx css_default_font_size(JSContext *realm)
{
    if (realm == NULL)
        DFAIL("css-fonts-4 §2.5 (Font size: the font-size property)'s `Initial: medium` was asked for an "
              "element whose document is NOT THE ACTIVE DOCUMENT of any navigable — a DOMParser document, an "
              "XHR `responseXML`, a `<template>`'s contents owner, or the document of a navigable that has "
              "been destroyed. The NUMBER does not need a viewport (unlike a viewport-percentage length, and "
              "unlike the ICB, the reader's default font size exists whether or not this document is on a "
              "screen); what needs a realm is the SOURCE KEY the fact is minted under, which "
              "core/frame/viewport.c keys on the document that read it so a child navigable's environment and "
              "its parent's are two questions rather than one. Every user agent answers a computed `font-size` "
              "for an element that generates no box, and CSSOM §9 reaches that answer through a conjunct it "
              "does not state: BUILD it over core/dom/element_view.h's `element_view_has_box`, which is the "
              "same one css_length.c's viewport-percentage arm and its `border-*-width` snap already name");
    return css_px_fact(CSS_ENV_DEFAULT_FONT_SIZE, realm);
}

double css_default_fixed_font_size_px(void)
{
    return CSS_DEFAULT_FIXED_FONT_SIZE_PX;
}

CssPx css_default_fixed_font_size(JSContext *realm)
{
    if (realm == NULL)
        DFAIL("the fixed-width default font size was asked for with no realm — the source key its fact is minted "
              "under is keyed on the document that read it, exactly as css_default_font_size's is");
    return css_px_fact(CSS_ENV_DEFAULT_FIXED_FONT_SIZE, realm);
}

bool css_absolute_size_keyword(const char *kw, size_t n)
{
    DCHECK(kw != NULL || n == 0,
           "css-fonts-4 §2.5's `<absolute-size>` membership was asked through a NULL span with a non-zero "
           "length — a keyword names its bytes inside the value it was serialized from, so an absent pointer "
           "is a caller that lost it");
    while (n > 0 && isspace((unsigned char)*kw)) { kw++; n--; }
    while (n > 0 && isspace((unsigned char)kw[n - 1])) n--;
    return css_keyword_set_has(CSS_KEYWORDS_ABSOLUTE_SIZE, css_keyword_of(kw, n));
}

CssPx css_font_size_for_keyword(JSContext *realm, CssKeyword kw, bool monospace, bool quirks)
{
    CssPx medium = monospace ? css_default_fixed_font_size(realm) : css_default_font_size(realm);
    int col, m = (int)medium.px, q = quirks ? 1 : 0;

    for (col = 0; col < 8 && CSS_FONT_SIZE_KEYWORDS[col] != kw; col++) { }
    if (col == 8)
        DFAILF("the table of font sizes was asked for keyword %u, which is not one of css-fonts-4 §2.5's eight "
               "`<absolute-size>` values; CSS_KEYWORDS_ABSOLUTE_SIZE and this table are one list", (unsigned)kw);
    DCHECK(medium.px == (double)m && m > 0,
           "a default font size is not a positive whole number of pixels — Blink's Settings hold it as an int, "
           "and the table's rows are indexed by it");
    /* `medium` is the reference middle value in every row, so its entry is the fact itself. */
    if (col == 3) return medium;
    if (m >= CSS_FS_TABLE_MIN && m <= CSS_FS_TABLE_MAX) {
        DCHECK(CSS_FONT_SIZE_TABLE[q][m - CSS_FS_TABLE_MIN][3] == m,
               "a row of the table of font sizes does not hold its own `medium` in the medium column");
        return css_px_atom(CSS_FONT_SIZE_OP[q][col], &medium, 1, CSS_FONT_SIZE_TABLE[q][m - CSS_FS_TABLE_MIN][col]);
    }
    return css_px_atom(CSS_FONT_SIZE_OP[q][col], &medium, 1,
                       fmax(CSS_FONT_SIZE_FACTORS[col] * m, CSS_FS_MIN_LOGICAL_PX));
}

CssPx css_relative_size_px(CssPx parent, bool larger)
{
    CssPx out = css_px_scale(parent, larger ? CSS_RELATIVE_SIZE_RATIO : 1.0 / CSS_RELATIVE_SIZE_RATIO);

    DCHECK(parent.px >= 0.0,
           "css-fonts-4 §2.5's `<relative-size>` was scaled off a NEGATIVE parent font size. §2.5 states the "
           "range in the property's own `Value:` line — `<length-percentage [0,∞]>` — and says it twice in "
           "words ('negative lengths are invalid', 'negative percentages are invalid'), so a negative value "
           "one node up is a declaration the grammar should have DROPPED rather than a size to scale");
    /* §2.5's OWN RANGE: "the specific ratio is unspecified, but should be around 1.2–1.5". It is asserted over
       the CONSTANT and not recovered from the quotient, because `smaller` divides and the round trip does not
       come back: 16 / 1.2 * 1.2 is 16.000000000000002, so a bound stated over the result would fire on an
       arithmetically correct answer — the false red CLAUDE.md §Testing describes, manufactured by the assert
       itself. What the RESULT is held to is the half floating point cannot blur, and it is the half an edit
       can actually get wrong: which of the two keywords made the text bigger. */
    DCHECK(CSS_RELATIVE_SIZE_RATIO >= CSS_RELATIVE_SIZE_MIN && CSS_RELATIVE_SIZE_RATIO <= CSS_RELATIVE_SIZE_MAX,
           "this user agent's `<relative-size>` ratio is outside the range css-fonts-4 §2.5 (Font size: the "
           "font-size property) states for one — 'the specific ratio is unspecified, but should be around "
           "1.2–1.5'. Pick a value inside it rather than widening the bounds: the range is the spec's, and a "
           "ratio outside it is a departure this user agent would owe a reason for");
    DCHECK(larger ? out.px >= parent.px : out.px <= parent.px,
           "css-fonts-4 §2.5's `larger` made an element's text SMALLER than its parent's, or `smaller` made it "
           "bigger — the two keywords have been exchanged. §2.5 defines them as a `<relative-size>` that "
           "increases or decreases the font size relative to the parent element, so the direction is the one "
           "part of this that is not a UA choice");
    return out;
}

bool css_font_affecting_property(const char *name)
{
    DCHECK(name != NULL, "css-values-4 §6.1.1's font-affecting question was asked about a NULL property name");
    /* §6.1.1 gives a DEFINITION — "properties that affect the font size or font metrics of an element" — and a
       note that its examples are not exhaustive, so this is that definition applied to the properties whose
       computed value core/css/css_computed_value.c derives, and it grows a row when that set does.
       `font-size` IS the font size, so it is the one row the set has today. The box-model lengths and the
       keyword-valued properties affect neither the size nor the metrics, which is why `margin: 1em` resolves
       against the ELEMENT's own computed font-size and `font-size: 1.2em` does not.
       `line-height` IS NOT ONE and will not be one when it is modelled — §6.1.1 says so in a parenthesis of
       its own ("the other font-relative lengths continue to resolve against the element's own metrics when
       used in line-height"), and its own example is the pair that makes the difference visible:
       `h1 { line-height: 1.2em }` is 20% greater than the font size of the h1 ELEMENT, while
       `h1 { font-size: 1.2em }` is 20% greater than the font size h1 elements INHERIT. */
    return strcmp(name, "font-size") == 0;
}

bool css_lh_affecting_property(const char *name)
{
    DCHECK(name != NULL, "css-values-4 §6.1.1's lh-affecting question was asked about a NULL property name");
    /* §6.1.1's own wording is "the line-height property OR font-affecting properties", so this is that
       disjunction and not a second table — the day the first list gains a row this one gains it too, which is
       the whole reason it is written over the predicate rather than beside it. */
    return strcmp(name, "line-height") == 0 || css_font_affecting_property(name);
}
