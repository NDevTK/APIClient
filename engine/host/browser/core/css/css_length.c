/* CSS Values and Units §6 — the `<length>` in CSS pixels. See css_length.h for why this is a component, for
   what each unit family resolves against, and why each one this engine cannot absolutize crashes with its OWN
   message. */
#include <ctype.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "core/css/css_length.h"
#include "core/css/css_math.h"
#include "core/frame/viewport.h"

static char *css_len_strdup(const char *s)
{
    char *out = strdup(s);
    CHECK(out != NULL, "cssom: OOM serializing a used length — a dropped one would read as undeclared");
    return out;
}

/* ---- the CSS pixel, and the environment fact it may derive from (see css_length.h) ------------------------ */

CssPx css_px(double px)
{
    CssPx out = { px, CSS_ENV_NONE, NULL };

    return out;
}

CssPx css_px_env(CssEnvFact fact, JSContext *realm, double px)
{
    CssPx out = css_px(px);

    /* ASSERTED BEFORE THE SHIFT, because the fact IS the bit position: a fact outside the enum would shift by
       more than the set is wide and the length would come out of this entry a function of nothing. */
    DCHECK((unsigned)fact < (unsigned)CSS_ENV_FACT_COUNT && realm != NULL,
           "a length was said to derive from an ENVIRONMENT FACT with no fact or no realm to name it — the two "
           "travel together because the mint at the JS boundary needs both (which viewport, and which of its "
           "dimensions), and a length carrying one without the other could not be turned into a domain");
    out.env = CSS_ENV_BIT(fact);
    out.realm = realm;
    return out;
}

/* THE UNION, WHICH IS THE WHOLE OF THE PROPAGATION — a length is a joint function of every fact either operand
   was a function of, and nothing here records WHICH function it is (that would be the transform-expression
   §Re-execution forbids; the example is right because the caller's real arithmetic produced `px`).
   THE REALM IS ONE, and it is asserted rather than merged: §10.1's containing-block chain and §6's snap are
   stated over one document, so two realms meeting in one length would mean a layout spanning documents — at
   which point the SET would have to carry a realm per fact, since `{viewport#3}` and `{viewport#7}` are two
   different questions with the same member name. */
static CssPx css_px_combine(CssPx a, CssPx b, double px)
{
    CssPx out = { px, a.env | b.env, a.realm ? a.realm : b.realm };

    DCHECK((a.env == CSS_ENV_NONE) == (a.realm == NULL) && (b.env == CSS_ENV_NONE) == (b.realm == NULL),
           "a length reached the arithmetic carrying facts without a realm, or a realm without facts — the two "
           "are written together by css_px_env and by nothing else, so one without the other is a length "
           "assembled field-by-field past that entry");
    DCHECK(a.realm == NULL || b.realm == NULL || a.realm == b.realm,
           "two lengths derived from the environments of DIFFERENT REALMS were combined into one. A child "
           "navigable's initial containing block is 300 CSS pixels wide and its parent's is 1280, so the two "
           "facts are different questions under one member name — and §10.1's containing-block chain walks one "
           "document, so this is arithmetic spanning two. BUILD the realm into the fact set (a realm per member "
           "rather than one per length) at the point a length is allowed to cross a document boundary");
    return out;
}

CssPx css_px_add(CssPx a, CssPx b)   { return css_px_combine(a, b, a.px + b.px); }
CssPx css_px_sub(CssPx a, CssPx b)   { return css_px_combine(a, b, a.px - b.px); }

CssPx css_px_scale(CssPx a, double k)
{
    CssPx out = a;

    out.px = a.px * k;
    return out;
}

/* THE LARGER (AND THE SMALLER) EXAMPLE, CARRYING BOTH OPERANDS' FACTS — which is why these are `css_px_combine`
   and not `a.px > b.px ? a : b`. Returning the winner whole would drop the loser's fact, and the loser is the
   arm the OTHER viewport takes: css-sizing §5's floor picks the declared length at 1280 and the four-term
   surround at 320, so a result that kept only the winner would report a length as environment-independent in
   exactly the world where it is not. Over-reporting the dependence forks a world the page might not have
   needed; under-reporting it deletes one it did, and CLAUDE.md's §Headless errs toward the first. */
CssPx css_px_max(CssPx a, CssPx b)
{
    return css_px_combine(a, b, a.px > b.px ? a.px : b.px);
}

CssPx css_px_min(CssPx a, CssPx b)
{
    return css_px_combine(a, b, a.px < b.px ? a.px : b.px);
}

CssPx css_px_mul(CssPx a, CssPx b)
{
    return css_px_combine(a, b, a.px * b.px);
}

CssPx css_px_div(CssPx a, CssPx b)
{
    DCHECK(b.px != 0.0,
           "a quotient of two lengths was taken with a ZERO divisor. Every algorithm that divides here states "
           "the non-zero branch itself — Intersection Observer §3.2.10 step 12 divides only 'if targetArea is "
           "non-zero' and answers 1 or 0 otherwise — so a zero arriving means the caller skipped its own "
           "branch rather than that this operation needs one");
    return css_px_combine(a, b, a.px / b.px);
}

/* CSS Values §6.2's ABSOLUTE UNITS, each as the spec's own definition of it in CSS pixels: "1in = 96px" is the
   anchor and the other five are exact fractions of the inch (1cm = 96/2.54, 1mm = 96/25.4, 1Q = 96/101.6,
   1pt = 96/72, 1pc = 16). Nothing here is a device measurement, which is why §Headless does not reach it. */
static const struct { const char *unit; double px; } CSS_ABSOLUTE[] = {
    { "px", 1.0 },
    { "in", 96.0 },
    { "cm", 96.0 / 2.54 },
    { "mm", 96.0 / 25.4 },
    { "q",  96.0 / 101.6 },
    { "pt", 96.0 / 72.0 },
    { "pc", 16.0 },
};

/* css-values-4 §6.1.1's FONT-RELATIVE units — ALL TWELVE, which its own section TITLE enumerates:
   "Font-relative Lengths: the em, rem, ex, rex, cap, rcap, ch, rch, ic, ric, lh, rlh units". §6.1.1 splits
   them into the LOCAL font-relative lengths, which "refer to the font metrics ... of the element on which they
   are used", and the ROOT font-relative lengths, which refer to the root element's — and every local one has
   an `r`-prefixed root twin, which is what makes the list twelve and not eight.
   FOUR OF THEM WERE MISSING AND THE COST WAS A WRONG CRASH, not a missing one: `rex`, `rcap`, `rch` and `ric`
   fell past this table and past css_len_is_viewport into `css_length_parse`'s last DFAIL, which says the unit
   is "a DIMENSION in a unit CSS Values §6 does not define as a length at all — an angle, a time, a frequency
   or a resolution". §6.1.1 defines all four as lengths, so that message sent the reader to look for a caller
   asking the wrong component when what is missing is a font metric. A crash that names the wrong absence is
   the stale-`DFAIL` failure with the coordinates still correct.
   THE TWELVE SPLIT BY WHERE THEIR MULTIPLIER COMES FROM, AND THE LINE IS THE SPEC'S OWN. §6.1.1 defines
   `em` as "the computed value of the font-size property of the element on which it is used" and `rem` as "the
   computed value of the em unit on the root element" — both are a computed `font-size` and nothing else. Six
   more are a FONT METRIC times one of those two sizes, and §6.1.1 states what MUST be assumed for each when
   the metric cannot be determined (an x-height is 0.5em, the "0" glyph is "0.5em wide by 1em tall", the "水"
   glyph is 1em), which core/css/font_metrics.h owns and which is a real value for a user agent with no glyph
   outlines rather than a stand-in. `cap`/`rcap` join them on the same arithmetic and a DIFFERENT footing:
   §6.1.1 fixes no cap-height, only that an undeterminable one takes "the font's ASCENT", and that ascent is a
   metric of the modelled face that core/css/font_metrics.h PICKS — so the product carries an environment fact
   where the three above carry none. All of them resolve through the ONE `CssFontMetrics` pair the caller
   holding the tree answers — one table, because the arithmetic is the same and only the base and the multiplier
   differ.
   AND `lh`/`rlh` COMPLETES THE TWELVE, on the same arithmetic and a base that is a PROPERTY rather than a
   metric: §6.1.1 makes it "the computed value of the line-height property of the element on which it is used,
   converting normal to an absolute length by using only the metrics of the first available font", so the
   caller answers css-inline-3 §5.1's computed value with CSS 2.1 §10.8.1's `AD` substituted for `normal`.
   WHICH element answers is §6.1.1's own rule for these two units alone — inside `line-height` they resolve
   against the PARENT's, which core/css/font_size_functions.h's second predicate decides from the property
   exactly as the font-affecting one does. There is no arm left in this file for a font-relative unit to reach
   past the table, which is why there is no list beside it any more. */
static const struct { const char *unit; CssFontMetric metric; } CSS_FONT_RELATIVE[] = {
    { "em",  CSS_FONT_METRIC_EM  }, { "rem", CSS_FONT_METRIC_REM },
    { "ex",  CSS_FONT_METRIC_EX  }, { "rex", CSS_FONT_METRIC_REX },
    { "ch",  CSS_FONT_METRIC_CH  }, { "rch", CSS_FONT_METRIC_RCH },
    { "ic",  CSS_FONT_METRIC_IC  }, { "ric", CSS_FONT_METRIC_RIC },
    { "cap", CSS_FONT_METRIC_CAP }, { "rcap", CSS_FONT_METRIC_RCAP },
    { "lh",  CSS_FONT_METRIC_LH  }, { "rlh", CSS_FONT_METRIC_RLH },
};

/* WHICH METRIC A UNIT NAMES, or false for one this file cannot route. It is a lookup and not a test followed
   by a second lookup, because the two would be one list that can disagree about a spelling. */
static bool css_len_font_metric_of(const char *unit, CssFontMetric *out)
{
    unsigned i;

    for (i = 0; i < sizeof(CSS_FONT_RELATIVE) / sizeof(CSS_FONT_RELATIVE[0]); i++)
        if (strcmp(CSS_FONT_RELATIVE[i].unit, unit) == 0) { *out = CSS_FONT_RELATIVE[i].metric; return true; }
    return false;
}

/* §6.1.2.2's VIEWPORT-PERCENTAGE units, in their DEFAULT spelling — the `v*` family, which §6.1.2.1 defines
   against the LARGE viewport size. The `sv*`, `lv*` and `dv*` variants are the same six names with one letter
   in front, which is the spec's own naming rule and is why they are recognised by it below rather than
   tabulated a second time. */
static const char *const CSS_VIEWPORT_RELATIVE[] = {
    "vw", "vh", "vi", "vb", "vmin", "vmax",
};

static bool css_len_in(const char *const *set, unsigned n, const char *unit)
{
    unsigned i;

    for (i = 0; i < n; i++)
        if (strcmp(set[i], unit) == 0) return true;
    return false;
}

#define CSS_LEN_N(set) (sizeof(set) / sizeof((set)[0]))

static bool css_len_is_viewport(const char *unit)
{
    return css_len_in(CSS_VIEWPORT_RELATIVE, CSS_LEN_N(CSS_VIEWPORT_RELATIVE), unit);
}

/* §6.1.2.1's `sv*` / `lv*` / `dv*` — the SMALL, LARGE and DYNAMIC viewport sizes, spelled as the default six
   names with one letter in front. */
static bool css_len_is_viewport_variant(const char *unit)
{
    if (unit[0] != 's' && unit[0] != 'l' && unit[0] != 'd') return false;
    return css_len_is_viewport(unit + 1);
}

/* §6.2's SEVEN ABSOLUTE UNITS, ASKED FROM OUTSIDE THE COMPUTED-VALUE CHAIN — see css_length.h for who asks and
   why it is not `css_length_parse`. The unit is compared ASCII-case-insensitively because a CSS unit identifier
   is, and the table's own spellings are lowercase; the comparison is written over a length rather than over a
   NUL, because a tokenizer hands its caller a span and copying it to ask this would be the copy that then has
   to be freed on every failure path. */
bool css_length_absolute_px(const char *unit, size_t unit_len, double n, double *px)
{
    unsigned i;
    size_t k;

    DCHECK(unit != NULL || unit_len == 0, "an absolute-unit test was handed a NULL span with a non-zero length");
    DCHECK(px != NULL, "an absolute-unit test was handed nowhere to write the length it converts");
    for (i = 0; i < CSS_LEN_N(CSS_ABSOLUTE); i++) {
        const char *u = CSS_ABSOLUTE[i].unit;

        if (strlen(u) != unit_len) continue;
        for (k = 0; k < unit_len; k++)
            if (tolower((unsigned char)unit[k]) != u[k]) break;
        if (k < unit_len) continue;
        *px = n * CSS_ABSOLUTE[i].px;
        return true;
    }
    return false;
}

/* §6.1.2's VIEWPORT-PERCENTAGE LENGTHS, absolutized: "the viewport-percentage lengths are relative to the size
   of the INITIAL CONTAINING BLOCK". `k` is the number divided by 100, because every one of these units is
   defined as 1% of a dimension. */
static CssPx css_len_viewport(JSContext *realm, const char *unit, double k)
{
    CssPx w, h;

    if (realm == NULL || !viewport_exists(realm))
        DFAIL("a VIEWPORT-PERCENTAGE length was absolutized for an element whose document is NOT BEING "
              "PRESENTED by any navigable — a DOMParser document, an XHR `responseXML`, a `<template>`'s "
              "contents owner, or the document of a navigable that has been destroyed. css-values §6.1.2 "
              "resolves these units against the INITIAL CONTAINING BLOCK and CSS 2.1 §10.1 gives that the "
              "dimensions of the VIEWPORT, so with no viewport there is no rectangle to be a percentage of. "
              "CSSOM §9 does not state this escape — its two conjuncts are the property's `Applies to:` line "
              "and the element's own computed `display`, and both can be true here — yet every user agent "
              "answers the computed value for an element that generates no box. BUILD §9's missing conjunct "
              "over the predicate that already decides it, core/dom/element_view.h's `element_view_has_box`, "
              "so the resolved value takes §9's computed-value escape before a length is ever absolutized "
              "(core/layout/used_value.c's uv_icb names the same one)");
    w = viewport_icb_width(realm);
    h = viewport_icb_height(realm);
    /* §6.1.2.1's ONE DIVERGENCE, asserted rather than assumed: "if the value of overflow or scrollbar-gutter on
       the root element in either axis would cause scrollbars to appear … unconditionally, the computed values
       of the viewport-percentage lengths in that axis are reduced in accordance with the initial containing
       block. Otherwise … the viewport-percentage lengths are sized assuming that scrollbars do not exist (EVEN
       IF THIS DIVERGES FROM THE INITIAL CONTAINING BLOCK)." This user agent renders no scroll bar, so the ICB
       is the viewport and the two sentences answer the same number — which is what makes resolving these units
       against §10.1's rectangle, and inheriting ITS environment fact, the whole rule rather than half of it.
       The day a scroll bar reduces the ICB the two separate: `vw` keeps following the unreduced viewport unless
       the root element's `overflow` forces the bar unconditionally, so that number becomes a fact of its own
       and this arm stops being able to borrow the ICB's. */
    DCHECK(w.px == viewport_width(realm) && h.px == viewport_height(realm),
           "css-values §6.1.2.1 sizes a viewport-percentage length assuming SCROLLBARS DO NOT EXIST, even where "
           "that diverges from the initial containing block, and the ICB this engine answers is no longer the "
           "whole viewport — so `vw` can no longer be a percentage of it. Read the root element's computed "
           "`overflow` in the axis: where it forces a scroll bar unconditionally the unit follows the reduced "
           "ICB, and otherwise it follows the unreduced viewport, which is then a SECOND environment fact "
           "beside CSS_ENV_ICB_WIDTH and needs its own row in core/frame/viewport.c's seam");
    if (strcmp(unit, "vw") == 0)   return css_px_scale(w, k);
    if (strcmp(unit, "vh") == 0)   return css_px_scale(h, k);
    /* §6.1.2.2: "vmin: equal to the smaller of vw and vh", "vmax: … the larger". BOTH axes are operands, so
       both facts reach the result and the answer is a JOINT function of them — which is exactly what a page
       branching on a `100vmin` box is branching on, a relation between the two viewport dimensions, and what a
       domain over either one alone could not say. Which axis is smaller is decided on the modelled viewport
       (media_query.h's layering) and the fact the loser carried survives the decision, because at another
       viewport it is the winner. */
    if (strcmp(unit, "vmin") == 0) return css_px_min(css_px_scale(w, k), css_px_scale(h, k));
    if (strcmp(unit, "vmax") == 0) return css_px_max(css_px_scale(w, k), css_px_scale(h, k));
    DCHECK(strcmp(unit, "vi") == 0 || strcmp(unit, "vb") == 0,
           "a unit reached the viewport-percentage resolution that is not one of §6.1.2.2's six — this arm and "
           "CSS_VIEWPORT_RELATIVE are one list and have come apart");
    DFAIL("`vi` and `vb` are 1% of the viewport IN THE BOX'S INLINE AND BLOCK AXIS (css-values §6.1.2.2), so "
          "which of the two ICB dimensions they are a percentage of is a fact about the element's WRITING MODE "
          "— css-writing-modes-4 §3.2 \"Block Flow Direction: the writing-mode property\" and §2.1 \"Specifying "
          "Directionality: the direction property\". BOTH VALUES ARE READABLE NOW: each is a `Computed value: "
          "specified value` line that core/css/css_computed_value.c models and core/css/css_defaulting.h "
          "inherits. WHAT IS MISSING IS THE MAPPING — css-writing-modes-4 §6.4 \"Abstract-to-Physical "
          "Mappings\", which turns the box's inline and block axes into the physical pair this resolution has "
          "to pick an ICB dimension from. BUILD §6.4 as its own component: CSSOM §9's used-value list carries "
          "the LOGICAL box-model properties and needs the same mapping before §10 can be asked about them "
          "either (core/css/css_property_applies.c crashes for them by name), and core/layout/flow_position.c "
          "crashes for a vertical writing mode because §9.4.1's rules are stated physically");
    return css_px(0.0);
}

/* THE DIMENSION'S UNIT, lowercased — CSS Syntax §4 makes a unit ASCII case-insensitive, and lexbor serializes
   the ident as the author wrote it, so `4PX` and `4px` arrive here as different bytes and are one value. */
#define CSS_LEN_UNIT_MAX 8

static bool css_len_unit_of(const char *s, char *out)
{
    size_t i;

    for (i = 0; s[i] != '\0'; i++) {
        if (i + 1 >= CSS_LEN_UNIT_MAX) return false;
        out[i] = (char)tolower((unsigned char)s[i]);
    }
    out[i] = '\0';
    return true;
}

/* §6's KEYWORD arm — the value is not a number at all, so what the caller compares is its TEXT (see
   css_length.h for why it is text and not an enum). Carried by value, so the length needs no free. */
static void css_len_keyword(CssLength *out, const char *kw)
{
    DCHECK(strlen(kw) < CSS_LENGTH_KEYWORD_MAX,
           "a length-valued property cascaded to a KEYWORD longer than any in a CSS length grammar. The longest "
           "one any of them admits is css-sizing's `-webkit-fill-available`, so this is either a declaration "
           "lexbor validated against a grammar this component does not know the property has, or a value that "
           "is not a keyword at all reaching the keyword arm");
    snprintf(out->keyword, sizeof out->keyword, "%s", kw);
}

/* ONE §6 UNIT, ABSOLUTIZED — the whole of the family split css_length.h describes, with each arm naming the
   component that is missing rather than the one beside it. `unit` is already lowercased and NUL-terminated.
   IT IS A FUNCTION AND NOT THE TAIL OF THE PARSE because css-values-4 §10's math functions reach the same
   table through a different door: a `calc(100vw - 2em)` resolves `100vw` and `2em` through exactly these arms,
   and the alternative is core/css/css_math.c carrying a second copy of the one table this file exists to be. */
static CssPx css_len_unit_px(JSContext *realm, const CssFontMetrics *font, const char *unit, double num)
{
    CssFontMetric metric;
    unsigned i;

    for (i = 0; i < CSS_LEN_N(CSS_ABSOLUTE); i++)
        if (strcmp(CSS_ABSOLUTE[i].unit, unit) == 0) return css_px(num * CSS_ABSOLUTE[i].px);
    /* §6.1.1's EIGHT RESOLVABLE FONT-RELATIVE UNITS, which are ONE arm because they are one multiplication.
       `em` is "the computed value of the font-size property of the element on which it is used" and `rem` is
       "the computed value of the em unit on the root element"; `ex`, `ch` and `ic` are a metric of the FIRST
       AVAILABLE FONT, which for a face with no glyph outlines is §6.1.1's own must-assume ratio of the em
       (core/css/font_metrics.h); and each `r`-prefixed twin is "the value of the <unit> unit ON THE ROOT
       ELEMENT". So every one of them is a base times this declaration's own multiplier, and the whole of the
       difference between them is WHICH base — which is the caller's walk (css_length.h), asked here and
       nowhere else because it needs the element and the tree this file deliberately does not hold.
       THE SCALE CARRIES THE BASE'S ENVIRONMENT FACTS, so a `margin: 1em` reaches the page as a function of the
       reader's default font size exactly as a `50vw` reaches it as a function of the viewport — and a
       `width: 20ch` does too, because the assumed ratio is a spec constant with no freedom in it and the only
       picked number in the product is the font size. The responsive ladder a `rem`- or `ch`-sized bundle
       builds on that number keeps its second arm. */
    if (css_len_font_metric_of(unit, &metric)) {
        CssPx base = font->resolve(font->ctx, metric);

        DCHECK(base.px >= 0.0,
               "a css-values-4 §6.1.1 font-relative unit was resolved against a NEGATIVE base. Every base this "
               "callback answers is a computed `font-size` or a non-negative ratio of one, and css-fonts-4 "
               "§2.5 (Font size: the font-size property) states that property's range in its own `Value:` "
               "line — `<length-percentage [0,∞]>` — and again in words twice, so a negative one is a "
               "declaration the grammar should have DROPPED rather than a length to be a multiple of");
        DCHECK(base.px == base.px && base.px * 0.0 == 0.0,
               "a css-values-4 §6.1.1 font-relative unit was resolved against a base that is not a FINITE "
               "number — every value css-fonts-4 §2.5 admits is an absolute length off a real declaration, a "
               "ratio of one, or §2.5.1's table entry, and §6.1.1's assumed metrics are finite ratios of "
               "those, so a NaN or an infinity is a derivation that lost an operand rather than a size this "
               "multiplication can use");
        return css_px_scale(base, num);
    }
    if (css_len_is_viewport(unit))
        return css_len_viewport(realm, unit, num / 100.0);
    else if (css_len_is_viewport_variant(unit))
        DFAIL("a length in one of css-values §6.1.2.1's SMALL, LARGE or DYNAMIC viewport-percentage units "
              "(`svh`, `lvw`, `dvh`, …). The default `v*` family resolves — it is 1% of the INITIAL CONTAINING "
              "BLOCK, which core/frame/viewport.h models — and these three do not, because each is a percentage "
              "of a DIFFERENT viewport size: the large one assumes every dynamically retractable UA interface "
              "retracted, the small one assumes them all expanded, and the dynamic one tracks them and is "
              "explicitly NOT STABLE while the viewport itself is unchanged. This engine models ONE viewport "
              "with no retractable interface, so the three coincide in the number — and answering all four "
              "families out of the ONE source key that number carries is exactly what must not happen: "
              "`100dvh === 100lvh` is the comparison a mobile bundle writes its viewport workaround around, and "
              "deciding it on the shared example deletes the arm where they differ. BUILD the three viewport "
              "sizes as their own PICKED facts in core/frame/viewport.c — a row each in its seam's table, by "
              "the same test viewport.h applies to `innerWidth` and the ICB, which reports the same number "
              "today and is still a separate fact for the same reason");
    else
        DFAIL("a length-valued property's value is a DIMENSION in a unit CSS Values §6 does not define as a "
              "length at all — an angle, a time, a frequency or a resolution. A property whose grammar admits "
              "one of those is not a box-model length, so this is a caller asking the wrong component, or "
              "lexbor's own validation admitting a declaration its grammar rejects");
    return css_px(0.0);
}

/* css-values-4 §10's MATH FUNCTIONS, answered over the table above. `css_math_eval` calls this only for a unit
   `css_length_is_length_unit` already admitted, so there is no "not a length" answer to give — the crash for a
   unit that cannot be absolutized is the same one a bare dimension reaches, in the same place. */
typedef struct { JSContext *realm; const CssFontMetrics *font; } CssLenMathCtx;

static CssPx css_len_math_length(void *ctx, double n, const char *unit, size_t unit_len)
{
    CssLenMathCtx *c = (CssLenMathCtx *)ctx;
    char lower[CSS_LEN_UNIT_MAX];
    size_t i;

    DCHECK(unit_len > 0 && unit_len < CSS_LEN_UNIT_MAX,
           "a math function's `<length>` leaf carries a unit longer than any CSS Values §6 defines. "
           "`css_math_eval` reaches this callback only for a unit `css_length_is_length_unit` has already "
           "admitted, and that entry refuses everything from CSS_LEN_UNIT_MAX bytes up, so one arriving here "
           "is those two tests having come apart");
    if (unit_len == 0 || unit_len >= CSS_LEN_UNIT_MAX) return css_px(0.0);
    for (i = 0; i < unit_len; i++) lower[i] = (char)tolower((unsigned char)unit[i]);
    lower[unit_len] = '\0';
    return css_len_unit_px(c->realm, c->font, lower, n);
}

CssLength css_length_parse(JSContext *realm, const CssFontMetrics *font, const char *value)
{
    CssLength out = { CSS_LENGTH_KEYWORD, css_px(0.0), 0.0, { '\0' } };
    char unit[CSS_LEN_UNIT_MAX];
    const char *p = value;
    char *end = NULL;
    double num;

    DCHECK(value != NULL, "a CSS length was parsed from a NULL value — the cascade answers every property this "
                          "engine models, so a NULL here is a cascade that stopped early");
    DCHECK(font != NULL && font->resolve != NULL,
           "a CSS length was absolutized with no source for css-values-4 §6.1.1's `em` and `rem`. Which "
           "computed `font-size` a font-relative unit means is CSS Cascade §7.2's inheritance walk over the "
           "flattened element tree, and this component holds no tree — so the pair is the CALLER's to answer "
           "and is required of every caller, not supplied where it happens to be convenient. It is consulted "
           "lazily, so a caller that knows its value carries no font-relative unit still passes it rather than "
           "deciding for this file which arm it will take");
    while (*p != '\0' && isspace((unsigned char)*p)) p++;
    num = strtod(p, &end);
    /* NOT A NUMBER AT ALL. Either a KEYWORD — `auto`, `none`, `min-content`, `normal` — whose text every
       caller that cares compares for itself (inventing an enum of every keyword every length-valued property
       admits would be a second copy of lexbor's own grammar), or a FUNCTION, which is a different thing and is
       told apart by the one character that distinguishes them in CSS Syntax's own tokenizer. */
    if (end == p) {
        if (strchr(p, '(') != NULL) {
            CssLenMathCtx mc;
            CssMathResolver res;
            CssMathValue v;

            mc.realm = realm;
            mc.font = font;
            res.length_px = css_len_math_length;
            res.ctx = &mc;
            res.realm = realm;
            /* §10.9.1 "Calculation Contexts": "Math functions always inherit the calculation context from
               wherever they're used." A length-valued property is §10.9's own worked example of a context that
               resolves percentages against a `<length>` ("such as in width, where <percentage> is resolved
               against a <length>"), so `<length-percentage>` is the production asked for and `calc(25% + 50px)`
               is valid here for exactly the reason `width: 25%` is. */
            if (css_math_eval(p, strlen(p), &res, CSS_MATH_PROD_LENGTH_PERCENTAGE, &v)) {
                /* §10.11 "Computed Value": "Where percentages are not resolved at computed-value time, they are
                   not resolved in math functions, e.g. calc(100% - 100% + 1px) resolves to calc(0% + 1px), not
                   to 1px." So a surviving percentage TERM is the whole test and the percentage's NUMBER is not
                   — §10.10.1 "Simplification"'s "zero-valued terms cannot be simply removed from a Sum" is why
                   `css_math_eval` answers `pct_term` beside `pct`, and reading `pct != 0` here would turn that
                   worked example's `calc(0% + 1px)` into the `1px` the sentence says it is not. */
                if (v.pct_term) {
                    out.kind = CSS_LENGTH_CALCULATED;
                    out.pct = v.pct;
                }
                else {
                    out.kind = CSS_LENGTH_ABSOLUTE;
                }
                out.px = v.num;
                return out;
            }
            DFAIL("a length-valued property's value is a FUNCTION that is not a math function resolving to a "
                  "`<length>` — `rgb(1, 2, 3)`, css-sizing's `fit-content()`, a `var()` substitution that "
                  "produced neither, or a math function whose type is failure (`calc(1px + 1s)`). Every one of "
                  "those is a declaration CSS Syntax DROPS, and the entry that decides it is "
                  "`css_length_is_length`, which now asks core/css/css_math.h the same question this arm just "
                  "asked. So a value reaching here is one that answered TRUE there and FALSE here, which means "
                  "the two are asking about different productions — or a caller that never asked at all, which "
                  "for a shorthand lexbor's registry does not carry is the whole of the validation there is");
        }
        css_len_keyword(&out, p);
        return out;
    }
    while (*end != '\0' && isspace((unsigned char)*end)) end++;
    if (*end == '\0') {
        /* §6: "for zero lengths the unit identifier is optional". A unitless NON-zero is not a length in any
           property this engine models — it is `line-height: 1.5`, whose own <number> grammar is that property's
           and not this one's. */
        DCHECK(num == 0.0,
               "a length-valued property's value is a UNITLESS NON-ZERO NUMBER. CSS Values §6 permits the "
               "unit to be omitted only for zero, so this is either a property whose grammar admits a bare "
               "<number> (`line-height`, `zoom`) reaching a caller that asked for a length, or a serializer "
               "that dropped a unit");
        out.kind = CSS_LENGTH_ABSOLUTE;
        out.px = css_px(0.0);
        return out;
    }
    if (*end == '%' && end[1] == '\0') {
        out.kind = CSS_LENGTH_PERCENTAGE;
        out.pct = num;
        return out;
    }
    if (!css_len_unit_of(end, unit)) {
        DFAIL("a length-valued property's value carries a DIMENSION whose unit is longer than any CSS unit "
              "identifier — CSS Values §6's longest is five characters (`dvmin`), so this is a value that is "
              "not a dimension reaching the dimension arm");
        return out;
    }
    out.kind = CSS_LENGTH_ABSOLUTE;
    out.px = css_len_unit_px(realm, font, unit, num);
    return out;
}

/* Is `unit` (already lowercased) one CSS Values §6 defines as a LENGTH unit — absolute, font-relative or
   viewport-percentage in any of its four families? All of them, because §6's production admits all of them and
   the ones this engine cannot absolutize are a missing component rather than a syntax error. */
static bool css_len_unit_known(const char *unit)
{
    unsigned i;

    for (i = 0; i < CSS_LEN_N(CSS_ABSOLUTE); i++)
        if (strcmp(CSS_ABSOLUTE[i].unit, unit) == 0) return true;
    {
        CssFontMetric metric;

        /* All twelve of §6.1.1's font-relative units are rows of the one table now, so this is the whole
           font-relative family and there is no second list beside it to fall out of step with. */
        if (css_len_font_metric_of(unit, &metric)) return true;
    }
    return css_len_is_viewport(unit) || css_len_is_viewport_variant(unit);
}

bool css_length_is_length_unit(const char *unit, size_t unit_len)
{
    char lower[CSS_LEN_UNIT_MAX];
    size_t i;

    DCHECK(unit != NULL || unit_len == 0,
           "§6's unit set was asked about through a NULL span with a non-zero length — a dimension token names "
           "its unit inside the buffer it was tokenized from, so an absent pointer is a caller that lost it");
    /* Every unit §6 defines is at most five bytes (`dvmin`), so one that does not fit is not one of them and
       the copy the comparison needs is never made for it. */
    if (unit_len == 0 || unit_len >= CSS_LEN_UNIT_MAX) return false;
    for (i = 0; i < unit_len; i++) lower[i] = (char)tolower((unsigned char)unit[i]);
    lower[unit_len] = '\0';
    return css_len_unit_known(lower);
}

/* §6's production over serialized text, with `want` naming which of the two questions below is being asked —
   both walks are identical apart from what a `<percentage>` and a math function answer, and writing them twice
   is how the two would come to disagree about `calc(2em)`. */
static bool css_len_is(const char *value, CssMathProduction want)
{
    const char *p = value;
    char *end = NULL;

    DCHECK(value != NULL, "the `<length>` grammar was asked about a NULL value — a declaration carries a value "
                          "by the time lexbor has serialized it, and an absent one is a caller that lost it");
    while (*p != '\0' && isspace((unsigned char)*p)) p++;
    (void)strtod(p, &end);
    /* Not a number token at all: a FUNCTION is the one remaining production a `<length>` can take (see the
       header), and every other spelling — a keyword, an identifier, a string — is not one. WHICH function is
       css-values-4 §10.9 "Type Checking"'s question and is now answerable: a math function IS a `<length>` when
       its type matches one, `rgb(1, 2, 3)` is not a math function at all, and `calc(1px + 1s)` is one whose
       type is failure. This is the entry css_length.h said "cannot tell them apart for the same reason". */
    /* The `(` is CSS Syntax's own one-character difference between a FUNCTION and an IDENT, and it is tested
       here so that `auto`, `none` and every other keyword answer without standing a tokenizer up. */
    if (end == p) return strchr(p, '(') != NULL && css_math_matches(p, strlen(p), want);
    while (*end != '\0' && isspace((unsigned char)*end)) end++;
    if (*end == '\0') return true;    /* §6's unitless zero; the parse above asserts on a unitless non-zero */
    if (*end == '%')
        /* §5.5's `<percentage>` is a SIBLING production of §6's `<length>` and never a `<length>`; it is one
           arm of `<length-percentage>`, which is the other question. */
        return want == CSS_MATH_PROD_LENGTH_PERCENTAGE && end[1] == '\0';
    return css_length_is_length_unit(end, strlen(end));
}

bool css_length_is_length(const char *value)
{
    return css_len_is(value, CSS_MATH_PROD_LENGTH);
}

bool css_length_is_length_percentage(const char *value)
{
    return css_len_is(value, CSS_MATH_PROD_LENGTH_PERCENTAGE);
}

/* CSS Values §6's SNAP A LENGTH AS A LINE WIDTH, in the spec's own three steps, over the DEVICE PIXEL the realm
   reports. The arithmetic is stated in device pixels because the algorithm is — a CSS pixel is not a whole
   number of them at a ratio of 1.5 — and the answer converts back, which is why a `1px` border is 1 CSS pixel
   at a ratio of 1 and two thirds of one at 1.5.
   THE RESULT DERIVES FROM THE RATIO, and that is the half this could not say while the computed-value path was
   text. viewport.h makes `devicePixelRatio` a PICKED environment fact for the reason it gives — `> 1` is the
   retina gate — so a snapped width is a function of a source a page can branch on, and reporting the modelled
   `1px` as a number the author's own declaration determined would delete that arm exactly as a bare 1280
   deletes the mobile one. The combine is what carries it, and it is also what makes a width that is ITSELF
   derived (`border-width: 1vw`) come out a function of the viewport AND of the ratio — two facts in one length,
   which is one joint domain and not a choice between them. */
CssPx css_length_snap_line_width(JSContext *realm, CssPx len)
{
    double ratio, device, snapped, out;

    DCHECK(len.px == len.px && len.px * 0.0 == 0.0,
           "CSS Values §6's snap-as-a-line-width was handed a length that is not a FINITE number — every "
           "border width reaching it is an absolute length off a real declaration or one of §3.3's three "
           "keywords, so a NaN or an infinity is a derivation that lost an operand");
    /* THE RATIO IS THE OUTPUT DEVICE'S AND THE KEY IS THE DOCUMENT'S, which is why a realm is required here
       even though CSSOM VIEW §4's `devicePixelRatio` is answered without asking which document is on the
       device: the domain this length carries is keyed on the document that read it, exactly as the viewport's
       is (core/frame/viewport.c's seam), and an element no navigable presents has no document to key on. */
    if (realm == NULL)
        DFAIL("a `border-*-width` was SNAPPED for an element whose document is not the active document of any "
              "navigable — a DOMParser document, an XHR `responseXML`, a `<template>`'s contents owner, or the "
              "document of a destroyed navigable. css-backgrounds-3 §3.3 snaps the COMPUTED value, so the "
              "answer derives from the device pixel ratio and needs a realm to key that fact on. Every user "
              "agent answers the computed value for an element that generates no box, and CSSOM §9 reaches "
              "that answer through a conjunct it does not state: BUILD it over "
              "core/dom/element_view.h's `element_view_has_box`, which is the same one the viewport-percentage "
              "arm above and core/layout/used_value.c's uv_icb name");
    ratio = viewport_device_pixel_ratio(realm);
    device = len.px * ratio;
    DCHECK(ratio > 0.0,
           "core/frame/viewport.h answered a DEVICE PIXEL RATIO that is not positive. CSSOM VIEW §4 defines it "
           "as the CSS pixel size divided by the device pixel size and returns 1 where there is no output "
           "device, so a zero or a negative one is a model with a device of no size");
    /* Step 1: "if len is an integer number of device pixels, do nothing." Step 2: "if the absolute value of len
       is greater than zero, but less than 1 device pixel, round it away from zero to 1 or -1 device pixel."
       Step 3: "if the absolute value of len is greater than 1 device pixel, round it towards zero to the
       nearest integer number of device pixels." */
    if (device == trunc(device))              snapped = device;
    else if (device > -1.0 && device < 1.0)   snapped = device > 0.0 ? 1.0 : -1.0;
    else                                      snapped = trunc(device);
    out = snapped / ratio;
    return css_px_combine(len, css_px_env(CSS_ENV_DEVICE_PIXEL_RATIO, realm, out), out);
}

/* THE SHORTEST DECIMAL THAT NAMES THIS DOUBLE, as its digits and its decimal exponent: the value is
   `D[0].D[1..ndig-1] x 10^e10`, signed separately. Printed at RISING PRECISION until strtod gives the value
   back, which is CSSOM §6.7.2's "in the shortest form possible" stated directly rather than a fixed `%g` that
   would report `4.0000000000000004px` for one length and truncate the next.
   THE `%e` FORM IS READ RATHER THAN PRINTED: it is the one printf conversion whose output separates the
   significand from the exponent, so the layout below is a positional rendering of the digits and never has to
   ask printf for one. */
static void css_num_shortest(double v, char *digits, int *ndig, int *e10)
{
    char sci[40];
    const char *p;
    int prec, n = 0;

    for (prec = 1; prec <= 17; prec++) {
        snprintf(sci, sizeof sci, "%.*e", prec - 1, v);
        if (strtod(sci, NULL) == v) break;
    }
    DCHECK(prec <= 17, "a double did not round-trip through 17 significant digits, which is more than IEEE 754 "
                       "binary64 needs — the printer and the parser disagree");
    for (p = sci + (*sci == '-' ? 1 : 0); *p != 'e'; p++)
        if (*p != '.') digits[n++] = *p;
    digits[n] = '\0';
    DCHECK(n == prec, "the significand printf wrote does not carry the number of digits it was asked for — the "
                      "layout below indexes them against that count");
    DCHECK(n == 1 || digits[n - 1] != '0',
           "the shortest round-tripping significand ends in a ZERO, so dropping that digit would name the same "
           "double in one character less and the search above stopped a digit late — CSSOM §6.7.2 asks for the "
           "shortest form and this one is not it");
    *ndig = n;
    *e10 = (int)strtol(p + 1, NULL, 10);
}

/* CSSOM §6.7.2's SERIALIZE A `<number>`, entire: "A base-ten number using digits 0-9 in the SHORTEST FORM
   POSSIBLE, using '.' to separate decimals (if any), ROUNDING THE VALUE IF NECESSARY TO NOT PRODUCE MORE THAN
   6 DECIMALS, preceded by '-' if it is negative", with its own note that "scientific notation is not used".
 * BOTH CLAUSES ARE THE RULE AND NEITHER IS OPTIONAL, and this used to implement the first alone. CSS's
 * `<number-token>` grammar admits an exponent on INPUT — CSS Syntax §4.3.3 consumes one — so `width: 1e-7px`
 * is a declaration a page can write, and there is no notation to write the answer back in: `%g` produces one
 * for every value whose exponent is below -4 or at or above its precision, which is where the assert that
 * REJECTED an exponent was firing. Rejecting it was the wrong half of the rule; the digits are laid out
 * POSITIONALLY instead, and the six-decimal clause is what bounds that layout from below.
 * PRINTING `%.6f` FOR EVERY VALUE WOULD ANSWER THE SECOND CLAUSE AND BREAK THE FIRST: the double nearest 1e23
 * is 99999999999999991611392, and `%.6f` writes exactly that while the shortest form that names it is a 1 with
 * twenty-three zeros after it. So the shortest form is found first, and only a form needing more than six
 * decimals is re-rounded — which puts the value below 1e11, since more than six decimals means the exponent is
 * below `ndig - 7`, so the fixed-point print that rounds it is bounded and the shortest form of the ROUNDED
 * value is what gets laid out.
 * `suffix` is the unit CSSOM §6.7.2 writes after the number, the only thing that differs between a `<length>`
 * and a `<percentage>`. The buffer holds the widest positional form a finite double can take: a sign, the 309
 * integer digits of DBL_MAX, a point, six decimals and the unit. */
#define CSS_NUM_MAX 336

static char *css_len_serialize(double v, const char *suffix)
{
    char digits[24], rounded[32], out[CSS_NUM_MAX];
    int ndig, e10, decimals, i, n = 0;

    DCHECK(v == v && v * 0.0 == 0.0,
           "a value that is not a FINITE number reached CSSOM §6.7.2's serializer. Every length and every "
           "percentage this engine reports is off a real declaration or arithmetic over such values, so a NaN "
           "or an infinity is a derivation that lost an operand rather than a value to print");
    css_num_shortest(v, digits, &ndig, &e10);
    if (ndig - 1 - e10 > 6) {
        snprintf(rounded, sizeof rounded, "%.6f", v);
        v = strtod(rounded, NULL);
        css_num_shortest(v, digits, &ndig, &e10);
        DCHECK(ndig - 1 - e10 <= 6,
               "a value rounded to six decimal places came back from the shortest-form search needing more "
               "than six — the fixed-point print and the round-trip search disagree about what the rounded "
               "value is");
    }
    decimals = ndig - 1 - e10;
    DCHECK(e10 <= 308 && decimals <= 6,
           "the two facts that bound §6.7.2's positional layout to the buffer sized from DBL_MAX's decimal "
           "width — a FINITE double's decimal exponent, and the six-decimal clause applied above — do not both "
           "hold, so the layout below would write past it");
    /* "preceded by `-` if it is NEGATIVE" is a statement about the NUMBER, and negative zero is zero: CSS has
       no signed zero to distinguish, and a value the six-decimal clause rounded down to nothing keeps the sign
       bit of the length it came from. `v < 0.0` is the question the sentence asks; the printed sign is not. */
    if (v < 0.0) out[n++] = '-';
    if (e10 < 0) {
        /* Wholly fractional: §6.7.2's leading zero, then the exponent's own zeros, then every digit. */
        out[n++] = '0';
        out[n++] = '.';
        for (i = 0; i < -e10 - 1; i++) out[n++] = '0';
        for (i = 0; i < ndig; i++) out[n++] = digits[i];
    }
    else {
        /* The integer part is the first e10 + 1 digit positions, padded with the zeros a positional rendering
           of a large exponent needs and the shortest significand does not carry. */
        for (i = 0; i <= e10; i++) out[n++] = i < ndig ? digits[i] : '0';
        if (decimals > 0) {
            out[n++] = '.';
            for (i = e10 + 1; i < ndig; i++) out[n++] = digits[i];
        }
    }
    for (i = 0; suffix[i] != '\0'; i++) out[n++] = suffix[i];
    out[n] = '\0';
    return css_len_strdup(out);
}

char *css_length_serialize_px(double px)   { return css_len_serialize(px, "px"); }
char *css_length_serialize_pct(double pct) { return css_len_serialize(pct, "%"); }

/* §10.13's Sum branch over the two-term residue — see css_length.h for why the shape is pinned rather than
   chosen. The percentage is the FIRST child (sort a calculation's children nodes puts a number, then a
   percentage, then the dimensions), so the " - " rule is asked only of the length. */
char *css_length_serialize_calc(double pct, double px)
{
    char *pct_s = css_length_serialize_pct(pct);
    /* "if child is a NEGATIVE numeric value, append ' - ' to s, then serialize the NEGATION of child as
       normal" — so the sign is carried by the operator and never by the number after it. */
    char *px_s = css_length_serialize_px(px < 0.0 ? -px : px);
    size_t n = strlen(pct_s) + strlen(px_s) + sizeof("calc( + )");
    char *out = malloc(n);

    CHECK(out != NULL, "cssom: OOM serializing css-values-4 §10.13's math-function residue — a dropped one "
                       "would read as an undeclared property rather than as a `calc()`");
    snprintf(out, n, "calc(%s %c %s)", pct_s, px < 0.0 ? '-' : '+', px_s);
    free(pct_s);
    free(px_s);
    return out;
}

/* ---- css-values-4 §10.11 "Computed Value"'s USED-VALUE-TIME SIMPLIFICATION (see css_length.h) ------------- */

CssPx css_length_resolve_pct(CssLength len, CssPx basis)
{
    DCHECK(len.kind == CSS_LENGTH_PERCENTAGE || len.kind == CSS_LENGTH_CALCULATED,
           "a value with NO percentage in it was handed css-values-4 §10.11's used-value-time resolution. The "
           "two kinds that carry one are the whole of what a basis means, and an absolute length resolved "
           "against a containing block would be scaled by a measure it does not depend on — so this is a "
           "caller that dispatched on something other than the kind");
    /* §10.10.1 "Simplification": a Sum's children are combined only "with other values that have IDENTICAL
       units", so a `<percentage>` residue holds NO length term at all — and this arm's single expression is
       only right if the pair's unused half is the additive identity WITH AN EMPTY FACT SET. A length term left
       there by a producer would be added silently; an environment fact left there would be UNIONED into the
       result's domain and fork a world the value does not depend on. */
    DCHECK(len.kind != CSS_LENGTH_PERCENTAGE || (len.px.px == 0.0 && len.px.env == CSS_ENV_NONE),
           "a `<percentage>` computed value carries a LENGTH term. css_length_parse writes the percentage arm "
           "over a zeroed pair and css-values-4 §5.5 \"Percentages: the <percentage> type\" has no second term "
           "to write, so this is a producer that set `pct` on a value it had already given a length");
    /* §5.6's "both values are converted to absolute lengths and added", as ONE expression — the scaled basis
       carries the basis's facts, the length term carries its own, and `css_px_add` is what makes the answer a
       joint function of both rather than of whichever operand was resolved last. */
    return css_px_add(css_px_scale(basis, len.pct / 100.0), len.px);
}
