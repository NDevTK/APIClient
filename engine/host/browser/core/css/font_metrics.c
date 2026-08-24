/* The first available font's metrics. See font_metrics.h for which face this is, for why three of §6.1.1's
   metrics are the spec's own numbers rather than this component's, and for what is deliberately absent. */
#include "check.h"
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
          "carries (its ascent, its descent, its line gap, the advance of any other glyph) is a measurement of "
          "a real face. BUILD the face: see font_metrics.h for why its arrival is also what turns these ratios "
          "into a PICKED environment fact needing a row in core/frame/viewport.c's seam");
    return 0.0;
}
