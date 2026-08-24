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
          "tall\", and an undeterminable ideographic advance measure is 1em — and every other metric a font "
          "carries (its descent, its line gap, the advance of any other glyph) is a measurement of a real face "
          "— the ASCENT below is this file's one picked number and takes its own entry for exactly that "
          "reason, because it answers a `CssPx` carrying a fact where these three answer a bare ratio carrying "
          "none. BUILD the rest of the face there, beside it");
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
   once `D` exists to be checked against it. THAT SECOND CHECK IS NOT WRITTEN HERE AND CANNOT BE: `D` has no
   reader yet (font_metrics.h), so §10.8's recommendation is an invariant this file will GAIN with it, not one
   it is keeping quiet about. */
#define FONT_METRICS_ASCENT_EM 0.8

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
