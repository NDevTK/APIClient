/* css-fonts-4 §2.5.1's absolute-size table and the number `medium` is. See font_size_functions.h for why the
   default font size is a PICKED environment fact, why the `<relative-size>` ratio is not one, and why §2.5.1's
   readability floor is asserted rather than clamped. */
#include <ctype.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "check.h"
#include "core/css/css_length.h"
#include "core/css/font_size_functions.h"
#include "core/frame/viewport.h"

/* THE ONE PICKED NUMBER, IN CSS PIXELS — what §2.5's `Initial: medium` computes to on this user agent. §2.5.1
   makes every other entry an exact ratio to it, so this is the single point the whole table moves with, and it
   is the only value in this file a reader has to weigh rather than read off the spec. */
#define CSS_DEFAULT_FONT_SIZE_PX 16.0

/* §2.5.1's SCALING FACTORS, as the exact fractions the table prints them as rather than as decimals: the table
   reads "3/5, 3/4, 8/9, 1, 6/5, 3/2, 2/1, 3/1" and 8/9 has no finite decimal, so a rounded copy would be a
   second answer that is always slightly wrong. The ORDER is the table's own, smallest first, which is what
   makes the readability floor below a question about the FIRST row rather than a scan. */
static const struct { const char *kw; double num, den; } CSS_ABSOLUTE_SIZE[] = {
    { "xx-small",  3.0, 5.0 },
    { "x-small",   3.0, 4.0 },
    { "small",     8.0, 9.0 },
    { "medium",    1.0, 1.0 },
    { "large",     6.0, 5.0 },
    { "x-large",   3.0, 2.0 },
    { "xx-large",  2.0, 1.0 },
    { "xxx-large", 3.0, 1.0 },
};

#define CSS_FS_N(set) (sizeof(set) / sizeof((set)[0]))

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

/* §2.5.1's closing guideline, as a NUMBER this file can be held to: "to preserve readability, an UA applying
   these guidelines should nevertheless avoid creating font sizes of less than 9 DEVICE PIXELS per EM unit". */
#define CSS_FONT_SIZE_READABLE_DEVICE_PX 9.0

/* A CSS KEYWORD, COMPARED THE WAY CSS COMPARES ONE — ASCII case-insensitively (CSS Syntax §4 makes a unit or
   keyword ident case-insensitive), over the surrounding white space a serialization may leave on a value. The
   span is the caller's own bytes: css_font_shorthand.c holds a token it has not copied, and requiring a
   NUL-terminated lowercased copy would be the allocation every failure path then has to free. */
static bool fs_kw_is(const char *v, size_t n, const char *kw)
{
    size_t k = strlen(kw), j;

    while (n > 0 && isspace((unsigned char)*v)) { v++; n--; }
    while (n > 0 && isspace((unsigned char)v[n - 1])) n--;
    if (n != k) return false;
    for (j = 0; j < k; j++)
        if ((char)tolower((unsigned char)v[j]) != kw[j]) return false;
    return true;
}

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
    /* §2.5.1's readability guideline, asserted at the picked number rather than clamped onto the answer — see
       font_size_functions.h. The SMALLEST entry is the one that decides it, and the table is in the spec's own
       order, so it is row zero and not a scan. */
    DCHECK(CSS_DEFAULT_FONT_SIZE_PX * (CSS_ABSOLUTE_SIZE[0].num / CSS_ABSOLUTE_SIZE[0].den) *
                   viewport_device_pixel_ratio(realm) >= CSS_FONT_SIZE_READABLE_DEVICE_PX,
           "this user agent's DEFAULT FONT SIZE no longer clears css-fonts-4 §2.5.1 (Absolute Size Keyword "
           "Mapping Table)'s readability guideline: `xx-small` — the table's smallest entry, at a scaling "
           "factor of 3/5 — now computes to fewer than 9 DEVICE PIXELS per EM unit, which §2.5.1 says a user "
           "agent applying these guidelines should avoid creating. The fix is the PICKED number, not a clamp: "
           "clamping the table would make its two smallest keywords compute the same length and stop it being "
           "eight ratios of one value. Raise the default, or state why this user agent departs from §2.5.1");
    return css_px_env(CSS_ENV_DEFAULT_FONT_SIZE, realm, CSS_DEFAULT_FONT_SIZE_PX);
}

bool css_absolute_size_keyword(const char *kw, size_t n)
{
    unsigned i;

    DCHECK(kw != NULL || n == 0,
           "css-fonts-4 §2.5's `<absolute-size>` membership was asked through a NULL span with a non-zero "
           "length — a keyword names its bytes inside the value it was serialized from, so an absent pointer "
           "is a caller that lost it");
    for (i = 0; i < CSS_FS_N(CSS_ABSOLUTE_SIZE); i++)
        if (fs_kw_is(kw, n, CSS_ABSOLUTE_SIZE[i].kw)) return true;
    return false;
}

CssPx css_absolute_size_px(JSContext *realm, const char *kw)
{
    CssPx medium;
    unsigned i;

    DCHECK(kw != NULL, "§2.5.1's table was asked for the entry of a NULL keyword");
    medium = css_default_font_size(realm);
    for (i = 0; i < CSS_FS_N(CSS_ABSOLUTE_SIZE); i++) {
        if (!fs_kw_is(kw, strlen(kw), CSS_ABSOLUTE_SIZE[i].kw)) continue;
        /* §2.5.1: "the medium value is used as the reference middle value", so `medium`'s own entry is the
           identity and the other seven are its exact ratios. The scale is over the `CssPx`, which is what
           carries CSS_ENV_DEFAULT_FONT_SIZE through to the page — an `<absolute-size>` keyword is a function
           of the reader's font preference exactly as a bare `medium` is. */
        return css_px_scale(medium, CSS_ABSOLUTE_SIZE[i].num / CSS_ABSOLUTE_SIZE[i].den);
    }
    DFAIL("§2.5.1's table was asked for the entry of a keyword that is not one of css-fonts-4 §2.5's eight "
          "`<absolute-size>` values (xx-small, x-small, small, medium, large, x-large, xx-large, xxx-large). "
          "css_absolute_size_keyword and this table are ONE list and have come apart, or a caller reached the "
          "value entry without asking the membership one first");
    return medium;
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
