/* The first available font's metrics. See font_metrics.h for which face this is, for why three of §6.1.1's
   metrics are the spec's own numbers rather than this component's, for why the ascent is the other kind of
   answer, and for what is deliberately absent. */
#include "check.h"
#include "core/css/css_length.h"
#include "core/css/font_metrics.h"

/* §6.1.1's THREE MUST-ASSUME VALUES, in the section's own words and in its own units. Each row is a MUST whose
   antecedent — that the user agent cannot determine the metric — is literally true of a face with no glyph
   outlines, so the table is a citation and not a calibration. They are three DIFFERENT numbers and the two
   that coincide do so by accident of the spec's own prose: an undeterminable x-height is 0.5em and an
   undeterminable "0" glyph is "0.5em wide by 1em tall", while an undeterminable ideographic advance is 1em.
   Deriving any of them from another — `ic` from `ch`'s height, `ex` from `ch`'s width — would be one sentence
   of §6.1.1 standing in for another, and the day a real face lands they separate. */
static const struct { FontMetricsEmRatio which; double em; } FONT_METRICS_ASSUMED[] = {
    { FONT_METRICS_X_HEIGHT,            0.5 },
    { FONT_METRICS_ZERO_ADVANCE_WIDTH,  0.5 },
    { FONT_METRICS_ZERO_ADVANCE_HEIGHT, 1.0 },
    { FONT_METRICS_WATER_ADVANCE,       1.0 },
};

double font_metrics_em_ratio(FontMetricsEmRatio which)
{
    unsigned i;

    for (i = 0; i < sizeof(FONT_METRICS_ASSUMED) / sizeof(FONT_METRICS_ASSUMED[0]); i++)
        if (FONT_METRICS_ASSUMED[i].which == which) return FONT_METRICS_ASSUMED[i].em;
    DFAIL("a metric of the first available font was asked for that css-values-4 §6.1.1 states no MUST-ASSUME "
          "value for, so this table has no row to answer it with and must not invent one. §6.1.1 fixes exactly "
          "three — an undeterminable x-height is 0.5em, an undeterminable \"0\" glyph is \"0.5em wide by 1em "
          "tall\", and every other metric a font carries (its line gap, the advance of any glyph other than "
          "the two §6.1.1 assumes) is a measurement of a real face — which is why the ASCENT and the DESCENT "
          "below are PICKED and take entries of their own, answering a `CssPx` that carries the fact it was "
          "picked where these three answer a bare ratio carrying none. BUILD the rest of the face there, "
          "beside them");
    return 0.0;
}

/* THE ONE PICKED NUMBER IN THIS FILE — CSS 2.1 §10.8.1's `A` as a multiple of the em, which is the form every
   font metric takes because §10.8.1 states it "for a given font AT A GIVEN SIZE" and a face's metrics scale
   with the size. It is the only value here a reader has to weigh rather than read off the spec.
   WHY THIS POINT AND NOT ANOTHER, stated so the next reader can disagree with a reason. One spec-fixed number
   bounds it from below and one recommendation bounds the face from above. §6.1.1 assumes an x-height of 0.5em
   when it cannot be measured, and no face's characteristic height above the baseline is below its x-height, so
   `A` is at least 0.5em — asserted below, which is what keeps the picked number and the spec-fixed one ONE
   COHERENT FACE rather than two. CSS 2.1 §10.8 recommends "a used value for 'normal' between 1.0 to 1.2", and
   §10.8.1 makes that value a function of `AD = A + D`; a Western text face spends roughly a quarter of that
   sum below the baseline, so an `A` near four fifths of the em is the middle of the range that satisfies both
   once `D` exists to be checked against it. THAT CHECK IS WRITTEN NOW, over the pair, in
   `font_metrics_normal_line_height_px` below — which is where the sum is formed and therefore the only place
   §10.8's sentence is about something this file holds.
   THE TWO NUMBERS ARE ONE FACE AND WERE PICKED TOGETHER. `D` is 0.3em, which puts `AD` at 1.1em — the exact
   midpoint of §10.8's recommended 1.0 to 1.2 — and puts 0.273 of the pair below the baseline, which is the
   "roughly a quarter" this paragraph reasoned from before `D` existed. A reader who wants to move either has
   two asserts to satisfy and both name their section. */
#define FONT_METRICS_ASCENT_EM  0.8
#define FONT_METRICS_DESCENT_EM 0.3

CssPx font_metrics_ascent_px(JSContext *realm, CssPx font_size)
{
    if (realm == NULL)
        DFAIL("CSS 2.1 §10.8.1 (Leading and half-leading)'s `A` was asked for with no realm. The NUMBER needs "
              "none — this user agent's installed face is the same face whether or not the document is being "
              "presented, which is why its row in core/frame/viewport.c's seam is not a presented one — but "
              "the SOURCE KEY the fact is minted under is keyed on the document that read it, so a child "
              "navigable's environment and its parent's stay two questions rather than one. The caller is a "
              "css-values-4 §6.1.1 unit resolving on an element, and every element has a node document; one "
              "that is no navigable's ACTIVE document reaches this exactly as core/css/font_size_functions.c's "
              "`medium` does and takes the same answer");
    DCHECK(FONT_METRICS_ASCENT_EM >= font_metrics_em_ratio(FONT_METRICS_X_HEIGHT),
           "the picked ascent of the first available font is BELOW the x-height css-values-4 §6.1.1 assumes "
           "for it. No face has a characteristic height above the baseline smaller than the height of its own "
           "lowercase letters, so the two numbers no longer describe one face — and the consequence is visible "
           "in the units §6.1.1 defines over them: an undeterminable cap-height falls back to the ascent, so a "
           "`1cap` box would come out SHORTER than a `1ex` one");
    /* THE PRODUCT IS FORMED HERE so the two facts are unioned by css_length.h's arithmetic rather than by
       whichever caller remembered to: the answer is a joint function of the reader's font size, which the
       operand already carries, and of this user agent's face. */
    return css_px_mul(font_size, css_px_env(CSS_ENV_FONT_ASCENT, realm, FONT_METRICS_ASCENT_EM));
}

CssPx font_metrics_normal_line_height_px(JSContext *realm, CssPx font_size)
{
    CssPx descent;

    if (realm == NULL)
        DFAIL("CSS 2.1 §10.8.1 (Leading and half-leading)'s `AD` was asked for with no realm — the same "
              "question the ascent above answers, for the same reason: the two metrics are a source key each, "
              "and a key is keyed on the document that read it");
    /* §10.8's OWN RECOMMENDATION, ASSERTED OVER THE PAIR — "we recommend a used value for 'normal' between
       1.0 to 1.2". It is a statement about the SUM, which is why it could not be checked where `A` was picked
       and is checked here, at the one place the sum is formed. A user agent is permitted to disagree with a
       recommendation; what it must not do is disagree by ACCIDENT, which is what an unasserted pair of picked
       numbers drifting apart would be. The bounds are inclusive because the spec's "between 1.0 to 1.2" names
       both endpoints as acceptable values rather than as strict limits. */
    DCHECK(FONT_METRICS_ASCENT_EM + FONT_METRICS_DESCENT_EM >= 1.0 &&
               FONT_METRICS_ASCENT_EM + FONT_METRICS_DESCENT_EM <= 1.2,
           "the first available font's picked `A` and `D` sum to a `normal` line height OUTSIDE the range CSS "
           "2.1 §10.8 recommends — \"we recommend a used value for 'normal' between 1.0 to 1.2\". Under 1.0 "
           "successive lines overlap at the default font size and over 1.2 every block of text on every page "
           "is loosely set, and either is a user agent disagreeing with the spec's own guidance by accident "
           "rather than by decision. Move one of the two numbers in core/css/font_metrics.c, or state the "
           "disagreement here on purpose");
    /* §10.8.1: "we also define AD = A + D, the distance from the top to the bottom". The sum is formed over
       the two lengths rather than over the two ratios so that BOTH facts reach the result — a page reads the
       ascent through `1cap` and the sum through `1lh`, and a domain naming only one of them would leave the
       arm where the other reader's face differs unexplored. */
    descent = font_metrics_descent_px(realm, font_size);
    return css_px_add(font_metrics_ascent_px(realm, font_size), descent);
}

CssPx font_metrics_descent_px(JSContext *realm, CssPx font_size)
{
    if (realm == NULL)
        DFAIL("CSS 2.2 §10.8.1 (Leading and half-leading)'s `D` was asked for with no realm — the same "
              "question the ascent above answers, for the same reason: the two metrics are a source key each, "
              "and a key is keyed on the document that read it");
    /* THE SUM'S BOUND IS ASSERTED WHERE THE SUM IS FORMED and not here, because §10.8's recommendation is a
       statement about `normal` and therefore about `AD` — a bound restated over one term would be this file
       deciding how the pair splits, which is exactly the free choice the two picked numbers ARE. What is
       asserted about `D` alone is the one thing §10.8.1's own definition fixes: it is a DEPTH BELOW the
       baseline, so a face whose depth is negative has its baseline outside its own em box and every A' / D'
       split below it comes out on the wrong side of the line. */
    DCHECK(FONT_METRICS_DESCENT_EM >= 0.0,
           "the first available font's picked `D` is NEGATIVE. CSS 2.2 §10.8.1 defines it as \"a depth below\" "
           "the baseline, so a negative one puts the baseline below the bottom of the face and makes every "
           "half-leading split in core/layout/line_box.c place a box above a line it sits below");
    return css_px_mul(font_size, css_px_env(CSS_ENV_FONT_DESCENT, realm, FONT_METRICS_DESCENT_EM));
}
