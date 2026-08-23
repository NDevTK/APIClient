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
   THE TWELVE ARE TWO GROUPS AND THEY NAME TWO DIFFERENT MISSING COMPONENTS, which is why they are two tables
   and two crashes. §6.1.1 defines `em` as "the computed value of the font-size property of the element on
   which it is used" and `rem` as "the computed value of the em unit on the root element" — both are a computed
   `font-size` and nothing else. The other ten are FONT METRICS: an x-height (`ex`/`rex`), a cap-height
   (`cap`/`rcap`), the advance measure of the "0" glyph (`ch`/`rch`) and of the "水" glyph (`ic`/`ric`), and a
   computed `line-height` with `normal` resolved "by using only the metrics of the first available font"
   (`lh`/`rlh`). Reporting all twelve as one absence made the second group's message name a component that is
   not what they need. */
static const char *const CSS_FONT_SIZE_RELATIVE[] = { "em", "rem" };
static const char *const CSS_FONT_METRIC_RELATIVE[] = {
    "ex", "rex", "cap", "rcap", "ch", "rch", "ic", "ric", "lh", "rlh",
};

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
              "(core/layout/used_value.c's uv_icb_width names the same one)");
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
          "— css-writing-modes §6's `writing-mode` and `direction`, whose mapping from logical to physical this "
          "engine does not have. Both properties are INHERITED, and CSS Cascade §7's step that would carry "
          "them is built now (core/css/css_defaulting.h) — what is missing is that neither is among the "
          "properties css_computed_value.c models, so there is no computed `writing-mode` and no computed "
          "`direction` to read. BOTH ARE `Computed value: specified keyword` lines, so each is a row of "
          "css_computed_models' as-specified arm plus a row of css_shorthand_complete_for. BUILD the two rows, "
          "then css-writing-modes §6's logical-to-physical mapping, which the logical box-model properties in "
          "CSSOM §9's own used-value list need before §10 can be asked about them either");
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

CssLength css_length_parse(JSContext *realm, const char *value)
{
    CssLength out = { CSS_LENGTH_KEYWORD, css_px(0.0), 0.0, { '\0' } };
    char unit[CSS_LEN_UNIT_MAX];
    const char *p = value;
    char *end = NULL;
    double num;
    unsigned i;

    DCHECK(value != NULL, "a CSS length was parsed from a NULL value — the cascade answers every property this "
                          "engine models, so a NULL here is a cascade that stopped early");
    while (*p != '\0' && isspace((unsigned char)*p)) p++;
    num = strtod(p, &end);
    /* NOT A NUMBER AT ALL. Either a KEYWORD — `auto`, `none`, `min-content`, `normal` — whose text every
       caller that cares compares for itself (inventing an enum of every keyword every length-valued property
       admits would be a second copy of lexbor's own grammar), or a FUNCTION, which is a different thing and is
       told apart by the one character that distinguishes them in CSS Syntax's own tokenizer. */
    if (end == p) {
        if (strchr(p, '(') != NULL)
            DFAIL("a length-valued property's value is a FUNCTION — `calc()`, `min()`, `max()`, `clamp()`, "
                  "css-sizing's `fit-content()`, a `var()` substitution that produced one, or a function that "
                  "is not a length at all and whose declaration the grammar should have DROPPED (`border-width: "
                  "rgb(1, 2, 3)` reaches here through css_length_is_length, which cannot tell them apart for "
                  "the same reason). css-values §10 "
                  "makes a math function a value in its "
                  "own right whose result is computed from operands in every unit this file knows and several "
                  "it crashes on, and lexbor parses one and serializes it back as TEXT, so it arrives here "
                  "looking like neither a dimension nor a keyword. BUILD the math-function grammar and its type "
                  "algebra: that is where `calc(100% - 2em)` splits into the PERCENTAGE half its caller "
                  "resolves against the containing block and the LENGTH half the crashes below are about, "
                  "it is what tells a math function from `rgb()`, and it is also where css-variables' "
                  "substitution has to have happened already");
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
    for (i = 0; i < CSS_LEN_N(CSS_ABSOLUTE); i++) {
        if (strcmp(CSS_ABSOLUTE[i].unit, unit) != 0) continue;
        out.kind = CSS_LENGTH_ABSOLUTE;
        out.px = css_px(num * CSS_ABSOLUTE[i].px);
        return out;
    }
    if (css_len_in(CSS_FONT_SIZE_RELATIVE, CSS_LEN_N(CSS_FONT_SIZE_RELATIVE), unit)) {
        DFAIL("an `em` or a `rem` reached css-values-4 §6.1.1's absolutization (Font-relative Lengths: the em, "
              "rem, ex, rex, cap, rcap, ch, rch, ic, ric, lh, rlh units). §6.1.1 makes `em` \"the computed "
              "value of the font-size property of the element on which it is used\" and `rem` \"the computed "
              "value of the em unit on the root element\", and no element in this engine has a computed "
              "`font-size` for either to resolve against. THE CASCADE'S INHERITANCE STEP IS NOT THE BLOCKER — "
              "CSS Cascade §7 is built (core/css/css_defaulting.h), it knows `font-size` is an inherited "
              "property, and it takes the parent's computed value for every property css_computed_value.c "
              "models. `font-size` is not one of them, and it is the one whose `Computed value:` line cannot "
              "be the as-specified arm: css-fonts-4 §2.5 (Font size: the font-size property) states it as `an "
              "absolute length` over a `Value:` of `<absolute-size> | <relative-size> | "
              "<length-percentage [0,∞]> | math` and a `Percentages:` line of `refer to parent element's font "
              "size` — so a percentage, an `em`, `larger`, `smaller` and each of §2.5.1's EIGHT "
              "`<absolute-size>` keywords (Absolute Size Keyword Mapping Table: xx-small through xxx-large, "
              "medium the reference value at a scaling factor of 1) resolve against a value one node up. BUILD "
              "`font-size` as a length-valued row of css_computed_models — the chain then IS §7.2's, one node "
              "at a time, with `css_computed_length` carrying it. THE ONE CASE THAT IS EASY TO GET WRONG IS "
              "IN §6.1.1 IN SO MANY WORDS: inside a FONT-AFFECTING PROPERTY on the element the unit refers to, "
              "\"the font-relative lengths resolve against the computed metrics of the PARENT element—or "
              "against the computed metrics corresponding to the initial values of the font and line-height "
              "properties, if the element has no parent\", so `div{font-size:1.2em}` is 1.2 x the INHERITED "
              "size and `html{font-size:2rem}` is 2 x the INITIAL one rather than a definition of itself. "
              "media_query.c resolves `em` against the INITIAL font size and is right to: §6.1.1's own last "
              "clause says that outside the context of an element the font-relative lengths refer to the "
              "metrics of those initial values, so there the initial value IS the spec's answer, and borrowing "
              "that number here would make it a stand-in for the chain");
        return out;
    }
    if (css_len_in(CSS_FONT_METRIC_RELATIVE, CSS_LEN_N(CSS_FONT_METRIC_RELATIVE), unit)) {
        DFAIL("a length in one of css-values-4 §6.1.1's FONT-METRIC units (`ex`, `rex`, `cap`, `rcap`, `ch`, "
              "`rch`, `ic`, `ric`, `lh`, `rlh`) reached absolutization. These are NOT a computed `font-size` "
              "and must not be built out of one as though they were: §6.1.1 defines `ex` as \"the used "
              "x-height of the first available font\", `cap` as \"the used cap-height\", `ch` as \"the used "
              "advance measure of the '0' (ZERO, U+0030) glyph\", `ic` as the same for the \"'水' (CJK water "
              "ideograph, U+6C34) glyph\", and `lh` as the computed `line-height` \"converting normal to an "
              "absolute length by using only the metrics of the first available font\" — five real font "
              "metrics, of which this engine has none, plus the `r`-prefixed twin of each measured on the root "
              "element. THREE OF THEM HAVE A NORMATIVE FALLBACK AND TWO DO NOT, which is the order to build "
              "them in: §6.1.1 says an x-height that is impossible to determine \"must be assumed\" to be "
              "0.5em, and a '0' glyph likewise \"must be assumed to be 0.5em wide by 1em tall\" (so `ch` falls "
              "back to 0.5em, and to 1em when typeset upright, which is `writing-mode` and `text-orientation` "
              "— both inherited and neither modelled), while an undeterminable cap-height falls back to \"the "
              "font's ascent\", which is itself a metric and not a fallback. So `ex`, `ch` and `ic` become "
              "buildable the moment `em` does, and `cap`, `lh` and `rlh` need core/frame's font records to "
              "exist first. BUILD the computed `font-size` (the `em` crash above names it) before any of "
              "this, and a FONT RECORD — ascent, x-height, cap-height and the two advance measures of the "
              "first available font — for the rest");
        return out;
    }
    if (css_len_is_viewport(unit)) {
        out.kind = CSS_LENGTH_ABSOLUTE;
        out.px = css_len_viewport(realm, unit, num / 100.0);
        return out;
    }
    if (css_len_is_viewport_variant(unit)) {
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
        return out;
    }
    DFAIL("a length-valued property's value is a DIMENSION in a unit CSS Values §6 does not define as a length "
          "at all — an angle, a time, a frequency or a resolution. A property whose grammar admits one of those "
          "is not a box-model length, so this is a caller asking the wrong component, or lexbor's own "
          "validation admitting a declaration its grammar rejects");
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
    return css_len_in(CSS_FONT_SIZE_RELATIVE, CSS_LEN_N(CSS_FONT_SIZE_RELATIVE), unit) ||
           css_len_in(CSS_FONT_METRIC_RELATIVE, CSS_LEN_N(CSS_FONT_METRIC_RELATIVE), unit) ||
           css_len_is_viewport(unit) || css_len_is_viewport_variant(unit);
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

bool css_length_is_length(const char *value)
{
    const char *p = value;
    char *end = NULL;

    DCHECK(value != NULL, "the `<length>` grammar was asked about a NULL value — a declaration carries a value "
                          "by the time lexbor has serialized it, and an absent one is a caller that lost it");
    while (*p != '\0' && isspace((unsigned char)*p)) p++;
    (void)strtod(p, &end);
    /* Not a number token at all: a FUNCTION is the one remaining production a `<length>` can take (see the
       header), and every other spelling — a keyword, an identifier, a string — is not one. */
    if (end == p) return strchr(p, '(') != NULL;
    while (*end != '\0' && isspace((unsigned char)*end)) end++;
    if (*end == '\0') return true;    /* §6's unitless zero; the parse above asserts on a unitless non-zero */
    if (*end == '%') return false;    /* a `<percentage>` is a sibling production, never a `<length>` */
    return css_length_is_length_unit(end, strlen(end));
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
              "arm above and core/layout/used_value.c's uv_icb_width name");
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
